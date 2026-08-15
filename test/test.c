/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <37183985@qq.com>
 *         芦浩然
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unit tests for the TaskHealth client library.
 *
 * Tests the client API only (register / unregister / heartbeat / error codes).
 * Alert detection is the daemon's job and is demonstrated in demo/, not
 * asserted here.
 *
 * Requires a running taskhealthd on the default socket for the tests that
 * perform a register round-trip.  Start it separately first:
 *
 *     ./taskhealthd
 *
 * Build:  make test
 * Run:    ./test
 */

#define _GNU_SOURCE
#include "../src/taskhealth.h"
#include "../src/taskhealth_mutex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCK_PATH  "/tmp/taskhealth.sock"

static int   g_failures;
static int   g_passed;

#define TASSERT(cond, msg) do {                                    \
	if (!(cond)) {                                                 \
		fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		g_failures++;                                              \
	} else {                                                       \
		g_passed++;                                                \
	}                                                              \
} while (0)

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

static void test_mutex_basic(void)
{
	printf("  [test] mutex init/lock/unlock/resolve\n");
	struct taskhealth_mutex m;

	taskhealth_mutex_init(&m, "test-mutex");

	const char *name = taskhealth_mutex_resolve(m.addr);
	TASSERT(name != NULL && strcmp(name, "test-mutex") == 0,
		"mutex resolve by addr");

	taskhealth_mutex_lock(&m);
	taskhealth_mutex_unlock(&m);

	taskhealth_mutex_destroy(&m);
	TASSERT(taskhealth_mutex_resolve(m.addr) == NULL,
		"mutex resolve cleared after destroy");
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
	printf("TaskHealth Unit Tests (client library)\n");
	printf("=======================================\n\n");

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
	test_mutex_basic();

	printf("\n===========================================\n");
	printf("  passed: %d  failed: %d\n", g_passed, g_failures);

	return g_failures > 0 ? 1 : 0;
}
