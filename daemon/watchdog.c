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
#include "watchdog.h"
#include "registry.h"
#include "probe.h"
#include "alert.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_t g_watchdog_tid;
static _Atomic bool g_running;
static int64_t g_check_interval_ms;

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void handle_exit(Entry *e)
{
	if (!e->alerted_exit) {
		e->alerted_exit = true;
		e->active = false;
		e->client_fd = -1;
		alert_emit(ALERT_EXIT, e, NULL, 0, 0, NULL);
	}
}

/*
 * Heartbeat timeout + deadlock + lock-wait detection.
 * Called only when gap_ms > 0 and heartbeat has timed out.
 * Reads /proc once, then makes both deadlock and lock-wait judgments.
 */
static void probe_and_detect(Entry *e, int64_t now)
{
	char wchan[64];
	bool has_wchan;
	char state;

	state     = probe_thread_state(e->pid, e->tid);
	has_wchan = probe_wchan(e->pid, e->tid, wchan, sizeof(wchan)) > 0;

	/* not blocked on futex → reset lock-wait tracking */
	if (!has_wchan || !strstr(wchan, "futex")) {
		e->lock_wait_start_ns = 0;
		return;
	}
	if (state != 'S' && state != 'D') {
		e->lock_wait_start_ns = 0;
		return;
	}

	/* thread is stuck on a futex — read address + resolve module */
	uintptr_t futex_addr  = probe_futex_addr(e->pid, e->tid);
	char     *futex_module = futex_addr ?
		probe_resolve_futex(e->pid, futex_addr) : NULL;

	/* ②a deadlock: heartbeat timeout + futex blocking */
	if (!e->alerted_deadlock) {
		e->alerted_deadlock = true;
		alert_emit(ALERT_DEADLOCK, e, wchan, futex_addr, 0, futex_module);
	}

	/* ②b lock-wait timeout (only when lock_hold_ms > 0) */
	if (e->lock_hold_ms > 0) {
		if (e->lock_wait_start_ns == 0)
			e->lock_wait_start_ns = now;

		int64_t waited = now - e->lock_wait_start_ns;
		if (waited > e->lock_hold_ms * 1000000LL) {
			if (!e->alerted_lock_wait) {
				e->alerted_lock_wait = true;
				alert_emit(ALERT_LOCK_WAIT, e, wchan, futex_addr,
					   waited / 1000000LL, futex_module);
			}
		}
	}

	free(futex_module);
}

static void *watchdog_thread(void *arg)
{
	(void)arg;

	while (atomic_load(&g_running)) {
		int64_t loop_start = now_ns();
		int i, capacity;

		registry_lock();
		Entry *entries = registry_entries(&capacity);

		for (i = 0; i < capacity; i++) {
			Entry *e = &entries[i];
			int alive;
			int64_t now, elapsed;

			if (!e->active)
				continue;

			/* ① exit detection (always active) */
			alive = probe_thread_alive(e->pid, e->tid);
			if (alive == 0) {
				handle_exit(e);
				continue;
			}
			if (alive < 0)
				continue;

			/* ② heartbeat timeout → probe_and_detect */
			if (e->gap_ms > 0) {
				now     = now_ns();
				elapsed = now - e->last_heartbeat_ns;
				if (elapsed > e->gap_ms * 1000000LL)
					probe_and_detect(e, now);
			}
		}
		registry_unlock();

		int64_t scan_elapsed = now_ns() - loop_start;
		int64_t sleep_ns = g_check_interval_ms * 1000000LL - scan_elapsed;
		if (sleep_ns > 0) {
			struct timespec ts = {
				.tv_sec  = sleep_ns / 1000000000LL,
				.tv_nsec = sleep_ns % 1000000000LL,
			};
			nanosleep(&ts, NULL);
		}
	}
	return NULL;
}

int watchdog_start(int64_t check_interval_ms)
{
	g_check_interval_ms = check_interval_ms > 0 ? check_interval_ms : 1000;
	atomic_store(&g_running, true);

	if (pthread_create(&g_watchdog_tid, NULL, watchdog_thread, NULL) != 0) {
		atomic_store(&g_running, false);
		return -1;
	}
	return 0;
}

void watchdog_stop(void)
{
	atomic_store(&g_running, false);
	pthread_join(g_watchdog_tid, NULL);
}

bool watchdog_running(void)
{
	return atomic_load(&g_running);
}
