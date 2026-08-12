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
#include "../src/taskhealth.h"
#include "../src/taskhealth_mutex.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define SOCK_PATH "/tmp/taskhealth-demo.sock"

static pid_t g_daemon_pid;

static int start_daemon(void)
{
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return -1; }
	if (pid == 0) {
		execl("./taskhealthd", "taskhealthd",
		      "-s", SOCK_PATH,
		      "-i", "500",
		      "-m", "64",
		      NULL);
		perror("execl taskhealthd");
		_exit(1);
	}
	g_daemon_pid = pid;
	sleep(1); /* wait for daemon to start */
	return 0;
}

static void stop_daemon(void)
{
	if (g_daemon_pid > 0) {
		kill(g_daemon_pid, SIGTERM);
		waitpid(g_daemon_pid, NULL, 0);
		g_daemon_pid = 0;
	}
	unlink(SOCK_PATH);
}

static int client_init(void)
{
	struct taskhealth_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.socket_path, SOCK_PATH, sizeof(cfg.socket_path) - 1);
	return taskhealth_init(&cfg);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scenario 1 — unexpected exit
 * ══════════════════════════════════════════════════════════════════════ */

static void *exit_thread(void *arg)
{
	(void)arg;
	int rc = taskhealth_register("exit-worker", 0, 0);
	if (rc != TASKHEALTH_OK) {
		fprintf(stderr, "  register exit-worker failed: %d\n", rc);
		return NULL;
	}
	sleep(1);
	pthread_exit(NULL);
	return NULL;
}

static void test_unexpected_exit(void)
{
	printf("\n=== Scenario 1: Unexpected Exit ===\n");
	printf("  daemon should alert UNEXPECTED_EXIT below:\n  ");

	pthread_t t;
	pthread_create(&t, NULL, exit_thread, NULL);
	pthread_detach(t);
	sleep(3);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scenario 2 — deadlock
 * ══════════════════════════════════════════════════════════════════════ */

static struct taskhealth_mutex g_mutex_a, g_mutex_b;

static void *deadlock_thread_a(void *arg)
{
	(void)arg;
	taskhealth_register("worker-A", 2000, 0);

	taskhealth_mutex_lock(&g_mutex_a);
	usleep(100000);
	taskhealth_mutex_lock(&g_mutex_b);
	taskhealth_mutex_unlock(&g_mutex_b);
	taskhealth_mutex_unlock(&g_mutex_a);
	return NULL;
}

static void *deadlock_thread_b(void *arg)
{
	(void)arg;
	taskhealth_register("worker-B", 2000, 0);

	taskhealth_mutex_lock(&g_mutex_b);
	usleep(100000);
	taskhealth_mutex_lock(&g_mutex_a);
	taskhealth_mutex_unlock(&g_mutex_a);
	taskhealth_mutex_unlock(&g_mutex_b);
	return NULL;
}

static void test_deadlock(void)
{
	printf("\n=== Scenario 2: Deadlock ===\n");
	printf("  daemon should alert DEADLOCK below:\n  ");

	taskhealth_mutex_init(&g_mutex_a, "lock-A");
	taskhealth_mutex_init(&g_mutex_b, "lock-B");

	pthread_t ta, tb;
	pthread_create(&ta, NULL, deadlock_thread_a, NULL);
	pthread_create(&tb, NULL, deadlock_thread_b, NULL);

	sleep(5);

	taskhealth_mutex_destroy(&g_mutex_a);
	taskhealth_mutex_destroy(&g_mutex_b);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scenario 3 — normal heartbeat (no alert)
 * ══════════════════════════════════════════════════════════════════════ */

static volatile int g_normal_running = 1;

static void *normal_thread(void *arg)
{
	(void)arg;
	taskhealth_register("normal-worker", 3000, 0);

	while (g_normal_running) {
		taskhealth_heartbeat();
		usleep(100000);
	}
	taskhealth_unregister();
	return NULL;
}

static void test_normal_heartbeat(void)
{
	printf("\n=== Scenario 3: Normal Heartbeat (no alert expected) ===\n");

	pthread_t t;
	pthread_create(&t, NULL, normal_thread, NULL);
	sleep(4);
	g_normal_running = 0;
	pthread_join(t, NULL);

	printf("  PASS (no alert above)\n");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scenario 4 — lock wait timeout
 * ══════════════════════════════════════════════════════════════════════ */

static struct taskhealth_mutex g_holder_lock;
static volatile int g_holder_running = 1;

static void *holder_thread(void *arg)
{
	(void)arg;
	taskhealth_mutex_lock(&g_holder_lock);
	while (g_holder_running)
		sleep(1);
	taskhealth_mutex_unlock(&g_holder_lock);
	return NULL;
}

static void *waiter_thread(void *arg)
{
	(void)arg;
	taskhealth_register("lock-waiter", 3000, 2000);
	taskhealth_mutex_lock(&g_holder_lock);
	taskhealth_mutex_unlock(&g_holder_lock);
	return NULL;
}

static void test_lock_wait_timeout(void)
{
	printf("\n=== Scenario 4: Lock Wait Timeout ===\n");
	printf("  daemon should alert LOCK_WAIT below:\n  ");

	taskhealth_mutex_init(&g_holder_lock, "shared-lock");

	pthread_t th, tw;
	pthread_create(&th, NULL, holder_thread, NULL);
	usleep(500000);
	pthread_create(&tw, NULL, waiter_thread, NULL);

	sleep(6);

	g_holder_running = 0;
	pthread_join(th, NULL);
	pthread_join(tw, NULL);

	taskhealth_mutex_destroy(&g_holder_lock);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Scenario 5 — auto name
 * ══════════════════════════════════════════════════════════════════════ */

static void *auto_name_thread(void *arg)
{
	(void)arg;
	int rc = taskhealth_register(NULL, 3000, 0);
	if (rc == TASKHEALTH_OK) {
		printf("  PASS: auto-name registered\n");
		taskhealth_heartbeat();
		sleep(1);
		taskhealth_heartbeat();
		taskhealth_unregister();
	} else {
		printf("  FAIL: auto-name register failed (%d)\n", rc);
	}
	return NULL;
}

static void test_auto_name(void)
{
	printf("\n=== Scenario 5: Auto Name (NULL/empty) ===\n");

	pthread_t t;
	pthread_create(&t, NULL, auto_name_thread, NULL);
	pthread_join(t, NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void)
{
	printf("TaskHealth Demo (daemon architecture)\n");
	printf("======================================\n");

	if (start_daemon() < 0)
		return 1;

	if (client_init() != TASKHEALTH_OK) {
		fprintf(stderr, "client init failed\n");
		stop_daemon();
		return 1;
	}

	test_auto_name();
	test_unexpected_exit();
	test_deadlock();
	test_normal_heartbeat();
	test_lock_wait_timeout();

	printf("\n=== Summary ===\n");
	printf("  Check the daemon stderr output above for alerts.\n");
	printf("  Expected: 1 UNEXPECTED_EXIT + 2 DEADLOCK + 1 LOCK_WAIT\n");

	taskhealth_shutdown();
	stop_daemon();
	return 0;
}
