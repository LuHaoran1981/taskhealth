/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <luhaoran@symthosm.com>
 *         芦浩然
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unit tests for TaskHealth client library (daemon architecture).
 * Requires taskhealthd binary in current directory.
 *
 * Build:  make test
 * Run:    ./test
 */

#define _GNU_SOURCE
#include "../src/taskhealth.h"
#include "../src/taskhealth_mutex.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCK_PATH  "/tmp/taskhealth-test.sock"

static int   g_failures;
static int   g_passed;
static pid_t g_daemon_pid;
static int   g_daemon_stderr_fd;
static char  g_daemon_log[65536];
static int   g_daemon_log_len;

#define TASSERT(cond, msg) do {                                    \
	if (!(cond)) {                                                 \
		fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		g_failures++;                                              \
	} else {                                                       \
		g_passed++;                                                \
	}                                                              \
} while (0)

/* ── daemon lifecycle ──────────────────────────────────────────────── */

static void start_daemon(int max_entries)
{
	int pipefd[2];
	char max_str[16];

	snprintf(max_str, sizeof(max_str), "%d", max_entries);

	if (pipe(pipefd) < 0) { perror("pipe"); return; }

	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return; }

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("./taskhealthd", "taskhealthd",
		      "-s", SOCK_PATH,
		      "-i", "500",
		      "-m", max_str,
		      NULL);
		perror("execl taskhealthd");
		_exit(1);
	}

	close(pipefd[1]);
	g_daemon_pid = pid;
	g_daemon_stderr_fd = pipefd[0];

	/* set non-blocking for reading daemon output */
	int flags = fcntl(g_daemon_stderr_fd, F_GETFL, 0);
	fcntl(g_daemon_stderr_fd, F_SETFL, flags | O_NONBLOCK);

	sleep(1); /* wait for daemon to start and bind */
}

static void stop_daemon(void)
{
	if (g_daemon_pid > 0) {
		kill(g_daemon_pid, SIGTERM);
		waitpid(g_daemon_pid, NULL, 0);
		g_daemon_pid = 0;
	}
	if (g_daemon_stderr_fd >= 0) {
		close(g_daemon_stderr_fd);
		g_daemon_stderr_fd = -1;
	}
	unlink(SOCK_PATH);
}

static void drain_daemon_output(void)
{
	char buf[4096];
	ssize_t n;
	while ((n = read(g_daemon_stderr_fd, buf, sizeof(buf) - 1)) > 0) {
		if (g_daemon_log_len + n < (int)sizeof(g_daemon_log) - 1) {
			memcpy(g_daemon_log + g_daemon_log_len, buf, (size_t)n);
			g_daemon_log_len += (int)n;
			g_daemon_log[g_daemon_log_len] = '\0';
		}
	}
}

static void clear_daemon_log(void)
{
	g_daemon_log[0] = '\0';
	g_daemon_log_len = 0;
	drain_daemon_output();
	g_daemon_log[0] = '\0';
	g_daemon_log_len = 0;
}

static int daemon_log_contains(const char *pattern)
{
	drain_daemon_output();
	return strstr(g_daemon_log, pattern) != NULL;
}

/* ── client helpers ─────────────────────────────────────────────────── */

static int client_init(void)
{
	struct taskhealth_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.socket_path, SOCK_PATH, sizeof(cfg.socket_path) - 1);
	return taskhealth_init(&cfg);
}

/* ── tests ──────────────────────────────────────────────────────────── */

static void test_version(void)
{
	printf("  [test] version string\n");
	const char *v = taskhealth_version();
	TASSERT(v != NULL, "version not NULL");
	TASSERT(strlen(v) > 0, "version not empty");
	TASSERT(v[0] >= '0' && v[0] <= '9', "version starts with digit");
}

static void test_shutdown_before_init(void)
{
	printf("  [test] shutdown before init (no crash)\n");
	taskhealth_shutdown();
	TASSERT(1, "shutdown without init no crash");
}

static void test_unregister_without_init(void)
{
	printf("  [test] unregister without init (no crash)\n");
	taskhealth_unregister();
	TASSERT(1, "unregister without init no crash");
}

static void test_heartbeat_without_init(void)
{
	printf("  [test] heartbeat without init (no crash)\n");
	taskhealth_heartbeat();
	TASSERT(1, "heartbeat without init no crash");
}

static void test_init_shutdown(void)
{
	printf("  [test] init/shutdown lifecycle\n");

	int rc = client_init();
	TASSERT(rc == TASKHEALTH_OK, "init OK");
	TASSERT(rc == 0, "init returns 0");

	rc = client_init();
	TASSERT(rc == TASKHEALTH_ERR_ALREADY_INIT, "double init rejected");

	taskhealth_shutdown();

	rc = client_init();
	TASSERT(rc == TASKHEALTH_OK, "re-init after shutdown OK");

	taskhealth_shutdown();
}

static void test_register_before_init(void)
{
	printf("  [test] register before init\n");
	int rc = taskhealth_register("no-init", 0, 0);
	TASSERT(rc == TASKHEALTH_ERR_NOT_INIT, "register before init rejected");
}

static void test_register_unregister(void)
{
	printf("  [test] register/unregister\n");

	int rc = client_init();
	TASSERT(rc == TASKHEALTH_OK, "init OK");

	rc = taskhealth_register("reg-test", 5000, 0);
	TASSERT(rc == TASKHEALTH_OK, "first register OK");

	rc = taskhealth_register("reg-test2", 0, 0);
	TASSERT(rc == TASKHEALTH_ERR_ALREADY_REG, "double register rejected");

	taskhealth_unregister();

	rc = taskhealth_register("reg-test3", 0, 0);
	TASSERT(rc == TASKHEALTH_OK, "re-register after unregister OK");

	taskhealth_unregister();
	taskhealth_shutdown();
}

static void test_heartbeat_without_register(void)
{
	printf("  [test] heartbeat without register (no crash)\n");
	client_init();
	taskhealth_heartbeat();
	TASSERT(1, "heartbeat unregistered no crash");
	taskhealth_shutdown();
}

static void test_null_name_autogen(void)
{
	printf("  [test] NULL name auto-generate\n");
	client_init();

	int rc = taskhealth_register(NULL, 0, 0);
	TASSERT(rc == TASKHEALTH_OK, "NULL name accepted (auto-generated)");

	taskhealth_unregister();

	rc = taskhealth_register("", 0, 0);
	TASSERT(rc == TASKHEALTH_OK, "empty name accepted (auto-generated)");

	taskhealth_unregister();
	taskhealth_shutdown();
}

static void test_negative_params_rejected(void)
{
	printf("  [test] negative params rejected\n");
	client_init();

	int rc = taskhealth_register("bad", -1, 0);
	TASSERT(rc == TASKHEALTH_ERR_INVALID_ARG, "negative gap rejected");

	rc = taskhealth_register("bad2", 0, -1);
	TASSERT(rc == TASKHEALTH_ERR_INVALID_ARG, "negative lock_hold rejected");

	taskhealth_shutdown();
}

static void test_duplicate_tid_rejected(void)
{
	printf("  [test] duplicate TID rejected\n");
	client_init();

	int rc = taskhealth_register("first", 0, 0);
	TASSERT(rc == TASKHEALTH_OK, "first register OK");

	/* same thread trying again */
	rc = taskhealth_register("second", 0, 0);
	TASSERT(rc == TASKHEALTH_ERR_ALREADY_REG, "duplicate TID rejected");

	taskhealth_unregister();
	taskhealth_shutdown();
}

/* ── detection tests (daemon must be running) ─────────────────────── */

static void *thread_exit(void *arg)
{
	(void)arg;
	taskhealth_register("test-exit", 0, 0);
	sleep(1);
	pthread_exit(NULL);
	return NULL;
}

static void test_unexpected_exit(void)
{
	printf("  [test] unexpected exit detection\n");
	clear_daemon_log();

	client_init();

	pthread_t t;
	pthread_create(&t, NULL, thread_exit, NULL);
	pthread_detach(t);

	sleep(3);
	drain_daemon_output();

	TASSERT(daemon_log_contains("UNEXPECTED_EXIT"), "daemon logged UNEXPECTED_EXIT");
	TASSERT(daemon_log_contains("test-exit"), "daemon log contains thread name");

	taskhealth_shutdown();
}

static void *thread_clean(void *arg)
{
	(void)arg;
	taskhealth_register("test-clean", 0, 0);
	sleep(1);
	taskhealth_unregister();
	return NULL;
}

static void test_clean_unregister_no_alert(void)
{
	printf("  [test] clean unregister = no false alert\n");
	clear_daemon_log();

	client_init();

	pthread_t t;
	pthread_create(&t, NULL, thread_clean, NULL);
	pthread_join(t, NULL);
	sleep(2);
	drain_daemon_output();

	TASSERT(!daemon_log_contains("test-clean"), "no exit alert for clean unregister");

	taskhealth_shutdown();
}

static volatile int g_hb_running;

static void *thread_hb(void *arg)
{
	(void)arg;
	taskhealth_register("test-hb", 3000, 0);
	while (g_hb_running) {
		taskhealth_heartbeat();
		usleep(100000);
	}
	taskhealth_unregister();
	return NULL;
}

static void test_normal_heartbeat(void)
{
	printf("  [test] normal heartbeat = no false alarm\n");
	clear_daemon_log();

	client_init();

	g_hb_running = 1;
	pthread_t t;
	pthread_create(&t, NULL, thread_hb, NULL);
	sleep(4);
	g_hb_running = 0;
	pthread_join(t, NULL);
	drain_daemon_output();

	TASSERT(!daemon_log_contains("DEADLOCK"), "no false deadlock on normal heartbeat");
	TASSERT(!daemon_log_contains("UNEXPECTED_EXIT"), "no false exit on normal heartbeat");

	taskhealth_shutdown();
}

/* ── deadlock test ─────────────────────────────────────────────────── */

static struct taskhealth_mutex g_ma, g_mb;

static void *dead_a(void *arg)
{
	(void)arg;
	taskhealth_register("dead-A", 2000, 0);
	taskhealth_mutex_lock(&g_ma);
	usleep(100000);
	taskhealth_mutex_lock(&g_mb);
	taskhealth_mutex_unlock(&g_mb);
	taskhealth_mutex_unlock(&g_ma);
	return NULL;
}

static void *dead_b(void *arg)
{
	(void)arg;
	taskhealth_register("dead-B", 2000, 0);
	taskhealth_mutex_lock(&g_mb);
	usleep(100000);
	taskhealth_mutex_lock(&g_ma);
	taskhealth_mutex_unlock(&g_ma);
	taskhealth_mutex_unlock(&g_mb);
	return NULL;
}

static void test_deadlock(void)
{
	printf("  [test] deadlock detection\n");
	clear_daemon_log();

	client_init();
	taskhealth_mutex_init(&g_ma, "lock-A");
	taskhealth_mutex_init(&g_mb, "lock-B");

	pthread_t ta, tb;
	pthread_create(&ta, NULL, dead_a, NULL);
	pthread_create(&tb, NULL, dead_b, NULL);

	sleep(5);
	drain_daemon_output();

	TASSERT(daemon_log_contains("DEADLOCK"), "daemon logged DEADLOCK");
	TASSERT(daemon_log_contains("dead-A") || daemon_log_contains("dead-B"),
		"daemon log contains thread name");

	taskhealth_mutex_destroy(&g_ma);
	taskhealth_mutex_destroy(&g_mb);
	taskhealth_shutdown();
}

/* ── lock wait timeout test ────────────────────────────────────────── */

static struct taskhealth_mutex g_lock;
static volatile int g_hold = 1;

static void *lock_holder(void *arg)
{
	(void)arg;
	taskhealth_mutex_lock(&g_lock);
	while (g_hold) sleep(1);
	taskhealth_mutex_unlock(&g_lock);
	return NULL;
}

static void *lock_waiter(void *arg)
{
	(void)arg;
	taskhealth_register("lock-waiter", 3000, 2000);
	taskhealth_mutex_lock(&g_lock);
	taskhealth_mutex_unlock(&g_lock);
	return NULL;
}

static void test_lock_wait_timeout(void)
{
	printf("  [test] lock wait timeout (gap_ms>0 + lock_hold_ms>0)\n");
	clear_daemon_log();

	client_init();
	taskhealth_mutex_init(&g_lock, "shared-lock");

	pthread_t th, tw;
	pthread_create(&th, NULL, lock_holder, NULL);
	usleep(500000);
	pthread_create(&tw, NULL, lock_waiter, NULL);

	sleep(6);
	drain_daemon_output();

	TASSERT(daemon_log_contains("LOCK_WAIT") || daemon_log_contains("DEADLOCK"),
		"daemon logged lock-wait or deadlock alert");

	g_hold = 0;
	pthread_join(th, NULL);
	pthread_join(tw, NULL);

	taskhealth_mutex_destroy(&g_lock);
	taskhealth_shutdown();
}

/* ── lock wait recovery (lock released before threshold) ───────────── */

static struct taskhealth_mutex g_rec_lock;
static volatile int g_rec_hold = 1;

static void *rec_holder(void *arg)
{
	(void)arg;
	taskhealth_mutex_lock(&g_rec_lock);
	usleep(200000);
	g_rec_hold = 0;
	taskhealth_mutex_unlock(&g_rec_lock);
	return NULL;
}

static void *rec_waiter(void *arg)
{
	(void)arg;
	taskhealth_register("rec-waiter", 3000, 5000);
	taskhealth_mutex_lock(&g_rec_lock);
	taskhealth_mutex_unlock(&g_rec_lock);
	taskhealth_unregister();
	return NULL;
}

static void test_lock_wait_recovery(void)
{
	printf("  [test] lock wait recovery (no stale alert)\n");
	clear_daemon_log();

	client_init();
	taskhealth_mutex_init(&g_rec_lock, "rec-lock");

	pthread_t th, tw;
	pthread_create(&th, NULL, rec_holder, NULL);
	usleep(100000);
	pthread_create(&tw, NULL, rec_waiter, NULL);

	pthread_join(th, NULL);
	pthread_join(tw, NULL);
	sleep(2);
	drain_daemon_output();

	TASSERT(!daemon_log_contains("LOCK_WAIT") && !daemon_log_contains("DEADLOCK"),
		"no alert when lock released before timeout");

	taskhealth_mutex_destroy(&g_rec_lock);
	taskhealth_shutdown();
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
	printf("TaskHealth Unit Tests (daemon architecture)\n");
	printf("===========================================\n\n");

	start_daemon(64);

	printf("── API Tests ──\n");
	test_version();
	test_shutdown_before_init();
	test_unregister_without_init();
	test_heartbeat_without_init();
	test_init_shutdown();
	test_register_before_init();
	test_register_unregister();
	test_heartbeat_without_register();
	test_null_name_autogen();
	test_negative_params_rejected();
	test_duplicate_tid_rejected();

	printf("\n── Detection Tests (daemon required) ──\n");
	test_unexpected_exit();
	test_clean_unregister_no_alert();
	test_normal_heartbeat();
	test_lock_wait_recovery();
	test_deadlock();
	test_lock_wait_timeout();

	stop_daemon();

	printf("\n===========================================\n");
	printf("  passed: %d  failed: %d\n", g_passed, g_failures);

	return g_failures > 0 ? 1 : 0;
}
