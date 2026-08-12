/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <luhaoran@symthosm.com>
 *         芦浩然
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include "alert.h"
#include "registry.h"
#include "server.h"
#include "watchdog.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_SOCK_PATH  "/tmp/taskhealth.sock"

static struct {
	char socket_path[256];
	int64_t check_interval_ms;
	int max_entries;
	bool daemonize;
	char alert_script[256];
} g_cfg = {
	.socket_path       = DEFAULT_SOCK_PATH,
	.check_interval_ms = 1000,
	.max_entries       = 4096,
	.daemonize         = false,
	.alert_script      = "",
};

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"  -s, --socket PATH    Socket path (default: %s)\n"
		"  -i, --interval MS    Watchdog scan interval in ms (default: 1000)\n"
		"  -m, --max-entries N  Max monitored threads (default: 4096)\n"
		"  -d, --daemonize      Run as daemon\n"
		"  -a, --alert-script   Alert script path\n"
		"  -v, --version        Print version and exit\n"
		"  -h, --help           Print this help\n",
		prog, DEFAULT_SOCK_PATH);
}

static int parse_args(int argc, char *argv[])
{
	static struct option long_opts[] = {
		{ "socket",       required_argument, 0, 's' },
		{ "interval",     required_argument, 0, 'i' },
		{ "max-entries",  required_argument, 0, 'm' },
		{ "daemonize",    no_argument,       0, 'd' },
		{ "alert-script", required_argument, 0, 'a' },
		{ "version",      no_argument,       0, 'v' },
		{ "help",         no_argument,       0, 'h' },
		{ 0, 0, 0, 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "s:i:m:da:vh",
				 long_opts, NULL)) != -1) {
		switch (c) {
		case 's':
			strncpy(g_cfg.socket_path, optarg,
				sizeof(g_cfg.socket_path) - 1);
			break;
		case 'i':
			g_cfg.check_interval_ms = (int64_t)atol(optarg);
			if (g_cfg.check_interval_ms < 100)
				g_cfg.check_interval_ms = 100;
			if (g_cfg.check_interval_ms > 60000)
				g_cfg.check_interval_ms = 60000;
			break;
		case 'm':
			g_cfg.max_entries = atoi(optarg);
			if (g_cfg.max_entries < 1 || g_cfg.max_entries > 65536)
				g_cfg.max_entries = 4096;
			break;
		case 'd':
			g_cfg.daemonize = true;
			break;
		case 'a':
			strncpy(g_cfg.alert_script, optarg,
				sizeof(g_cfg.alert_script) - 1);
			break;
		case 'v':
			printf("taskhealthd %s\n", TASKHEALTH_VERSION);
			exit(0);
		case 'h':
			print_usage(argv[0]);
			exit(0);
		default:
			print_usage(argv[0]);
			return -1;
		}
	}
	return 0;
}

static void daemonize(void)
{
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); exit(1); }
	if (pid > 0) _exit(0);

	setsid();

	pid = fork();
	if (pid < 0) { perror("fork"); exit(1); }
	if (pid > 0) _exit(0);

	umask(022);
	chdir("/");

	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
}

static void stop_handler(int sig)
{
	(void)sig;
	server_request_stop();
}

static void setup_signals(void)
{
	struct sigaction sa_stop = {
		.sa_handler = stop_handler,
		.sa_flags   = SA_RESTART,
	};
	sigemptyset(&sa_stop.sa_mask);
	sigaction(SIGTERM, &sa_stop, NULL);
	sigaction(SIGINT,  &sa_stop, NULL);
	sigaction(SIGQUIT, &sa_stop, NULL);

	/* block harmless signals */
	struct sigaction sa_ign = {
		.sa_handler = SIG_IGN,
		.sa_flags   = SA_RESTART,
	};
	sigemptyset(&sa_ign.sa_mask);
	sigaction(SIGPIPE, &sa_ign, NULL);
	sigaction(SIGHUP,  &sa_ign, NULL);
	sigaction(SIGCHLD, &sa_ign, NULL);
	sigaction(SIGUSR1, &sa_ign, NULL);
	sigaction(SIGUSR2, &sa_ign, NULL);
}

int main(int argc, char *argv[])
{
	if (parse_args(argc, argv) < 0)
		return 1;

	setup_signals();

	if (g_cfg.daemonize)
		daemonize();

	fprintf(stderr, "[taskhealthd] starting (v%s) socket=%s interval=%ldms\n",
		TASKHEALTH_VERSION, g_cfg.socket_path,
		(long)g_cfg.check_interval_ms);

	if (registry_init(g_cfg.max_entries) < 0) {
		fprintf(stderr, "[taskhealthd] registry init failed\n");
		return 1;
	}

	alert_init(g_cfg.alert_script);

	if (server_init(g_cfg.socket_path) < 0) {
		fprintf(stderr, "[taskhealthd] server init failed (socket=%s)\n",
			g_cfg.socket_path);
		unlink(g_cfg.socket_path);
		registry_destroy();
		return 1;
	}

	if (watchdog_start(g_cfg.check_interval_ms) < 0) {
		fprintf(stderr, "[taskhealthd] watchdog start failed\n");
		server_stop();
		unlink(g_cfg.socket_path);
		registry_destroy();
		return 1;
	}

	/* main loop */
	server_run();

	watchdog_stop();
	server_stop();
	registry_destroy();
	unlink(g_cfg.socket_path);

	fprintf(stderr, "[taskhealthd] stopped\n");
	return 0;
}
