// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Copyright (c) 2020 Wenbo Zhang
//
// Based on filelife(8) from BCC by Brendan Gregg & Allan McAleavy.
// 20-Mar-2020   Wenbo Zhang   Created this.
// 13-Nov-2022   Rong Tao      Check btf struct field for CO-RE and add vfs_open()
// 23-Aug-2023   Rong Tao      Add vfs_* 'struct mnt_idmap' support.(CO-RE)
// 08-Nov-2023   Rong Tao      Support unlink failed
#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "compat.h"
#include "filelife.h"
#include "filelife.skel.h"
#include "btf_helpers.h"
#include "trace_helpers.h"


static volatile sig_atomic_t exiting = 0;

static struct env {
	pid_t pid;
	bool full_path;
	bool verbose;
} env = { };

const char *argp_program_version = "filelife 0.1";
const char *argp_program_bug_address =
	"https://github.com/iovisor/bcc/tree/master/libbpf-tools";
const char argp_program_doc[] =
"Trace the lifespan of short-lived files.\n"
"\n"
"USAGE: filelife  [--help] [-p PID]\n"
"\n"
"EXAMPLES:\n"
"    filelife         # trace all events\n"
"    filelife -F      # trace full-path of file\n"
"    filelife -p 123  # trace pid 123\n";

static const struct argp_option opts[] = {
	{ "pid", 'p', "PID", 0, "Process PID to trace", 0 },
	{ "full-path", 'F', NULL, 0, "Show full path", 0 },
	{ "verbose", 'v', NULL, 0, "Verbose debug output", 0 },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help", 0 },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	int pid;

	switch (key) {
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'v':
		env.verbose = true;
		break;
	case 'p':
		errno = 0;
		pid = strtol(arg, NULL, 10);
		if (errno || pid <= 0) {
			fprintf(stderr, "invalid PID: %s\n", arg);
			argp_usage(state);
		}
		env.pid = pid;
		break;
	case 'F':
		env.full_path = true;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_int(int signo)
{
	exiting = 1;
}

int handle_event(void *ctx, void *data, size_t data_sz)
{
	struct event e;
	char ts[32];

	if (data_sz < sizeof(e)) {
		printf("Error: packet too small\n");
		return -EINVAL;
	}
	/* Copy data as alignment in the ring buffer isn't guaranteed. */
	memcpy(&e, data, sizeof(e));

	str_timestamp("%H:%M:%S", ts, sizeof(ts));
	printf("%-8s %-6d %-16s %-7.2f ",
	       ts, e.tgid, e.task, (double)e.delta_ns / 1000000000);
	if (env.full_path) {
		print_full_path(&e.fname);
		printf("\n");
	} else
		printf("%s\n", e.fname.pathes);
	return 0;
}

void handle_lost_events(void *ctx, int cpu, __u64 lost_cnt)
{
	fprintf(stderr, "lost %llu events on CPU #%d\n", lost_cnt, cpu);
}

int main(int argc, char **argv)
{
	LIBBPF_OPTS(bpf_object_open_opts, open_opts);
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct bpf_buffer *buf = NULL;
	struct filelife_bpf *obj;
	int err;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	libbpf_set_print(libbpf_print_fn);

	err = ensure_core_btf(&open_opts);
	if (err) {
		fprintf(stderr, "failed to fetch necessary BTF for CO-RE: %s\n", strerror(-err));
		return 1;
	}

	obj = filelife_bpf__open_opts(&open_opts);
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		return 1;
	}

	/* initialize global data (filtering options) */
	obj->rodata->targ_tgid = env.pid;
	obj->rodata->full_path = env.full_path;

	if (!kprobe_exists("security_inode_create"))
		bpf_program__set_autoload(obj->progs.security_inode_create, false);

	err = filelife_bpf__load(obj);
	if (err) {
		fprintf(stderr, "failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	err = filelife_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	printf("Tracing the lifespan of short-lived files ... Hit Ctrl-C to end.\n");
	printf("%-8s %-6s %-16s %-7s %s\n", "TIME", "PID", "COMM", "AGE(s)", "FILE");

	buf = bpf_buffer__new(obj->maps.events, obj->maps.heap);
	if (!buf) {
		err = -errno;
		fprintf(stderr, "failed to create ring/perf buffer: %d", err);
		goto cleanup;
	}

	err = bpf_buffer__open(buf, handle_event, handle_lost_events, NULL);
	if (err) {
		err = -errno;
		fprintf(stderr, "failed to open ring/perf buffer: %d\n", err);
		goto cleanup;
	}

	if (signal(SIGINT, sig_int) == SIG_ERR) {
		fprintf(stderr, "can't set signal handler: %s\n", strerror(errno));
		err = 1;
		goto cleanup;
	}

	while (!exiting) {
		err = bpf_buffer__poll(buf, POLL_TIMEOUT_MS);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "error polling ring/perf buffer: %s\n", strerror(-err));
			goto cleanup;
		}
		/* reset err to return 0 if exiting */
		err = 0;
	}

cleanup:
	bpf_buffer__free(buf);
	filelife_bpf__destroy(obj);
	cleanup_core_btf(&open_opts);

	return err != 0;
}
