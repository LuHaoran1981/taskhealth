/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <37183985@qq.com>
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
static int64_t g_offline_ttl_ns = TASKHEALTH_OFFLINE_TTL_NS;

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* §3.4 + §8.4: server hands us an entry with client_fd == -1; this
 * means the socket was closed without MSG_SHUTDOWN → process crash.
 * We alert UNEXPECTED_EXIT, mark the entry as alerted, and add every
 * such thread to the offline-thread table + the process to the
 * offline-process table. */
static void handle_process_crash(Entry *e, int64_t now)
{
	if (e->alerted_exit) return;

	alert_emit(ALERT_EXIT, e, NULL, 0, 0, NULL, NULL);

	registry_offline_thread_add(e->proc_name, e->name,
				    e->pid, e->tid, now);
	registry_offline_process_add(e->proc_name, e->pid, now);

	e->alerted_exit = true;
	e->active       = false;
}

/* §3.4: tgkill returned ESRCH.  Socket is still alive → only the
 * thread is gone (process kept running).  Drop just that thread. */
static void handle_exit(Entry *e, int64_t now)
{
	if (e->alerted_exit) return;

	alert_emit(ALERT_EXIT, e, NULL, 0, 0, NULL, NULL);
	registry_offline_thread_add(e->proc_name, e->name,
				    e->pid, e->tid, now);

	e->alerted_exit = true;
	e->active       = false;
}

/* Same /proc read feeds both deadlock and lock-wait decisions. */
static void probe_and_detect(Entry *e, int64_t now)
{
	char wchan[64];
	bool has_wchan;
	char state;

	state     = probe_thread_state(e->pid, e->tid);
	has_wchan = probe_wchan(e->pid, e->tid, wchan, sizeof(wchan)) > 0;

	if (!has_wchan || !strstr(wchan, "futex")) {
		e->lock_wait_start_ns = 0;
		return;
	}
	if (state != 'S' && state != 'D') {
		e->lock_wait_start_ns = 0;
		return;
	}

	uintptr_t futex_addr   = e->wait_futex_addr;   /* cooperative */
	if (!futex_addr)
		futex_addr = probe_futex_addr(e->pid, e->tid);  /* /proc fallback */
	char *futex_module = futex_addr ?
		probe_resolve_futex(e->pid, futex_addr) : NULL;

	char lock_name[TASKHEALTH_NAME_LEN] = "";
	int  have_lock = futex_addr ?
		registry_mutex_resolve(e->client_fd >= 0 ? e->client_fd : -1,
				       (uint64_t)futex_addr,
				       lock_name, sizeof(lock_name)) : 0;

	/* ②a deadlock: heartbeat timeout + futex blocking */
	if (!e->alerted_deadlock) {
		e->alerted_deadlock = true;
		alert_emit(ALERT_DEADLOCK, e, wchan, futex_addr, 0, futex_module,
			   have_lock ? lock_name : NULL);
	}

	/* ②b lock-wait timeout */
	if (e->lock_hold_ms > 0) {
		if (e->lock_wait_start_ns == 0)
			e->lock_wait_start_ns = now;
		int64_t waited = now - e->lock_wait_start_ns;
		if (waited > e->lock_hold_ms * 1000000LL) {
			if (!e->alerted_lock_wait) {
				e->alerted_lock_wait = true;
				alert_emit(ALERT_LOCK_WAIT, e, wchan, futex_addr,
					   waited / 1000000LL, futex_module,
					   have_lock ? lock_name : NULL);
			}
		}
	}

	free(futex_module);
}

static void *watchdog_thread(void *arg)
{
	(void)arg;

	int capacity = 0;
	registry_lock();
	registry_entries(&capacity);
	registry_unlock();

	Entry *snapshot = calloc((size_t)capacity, sizeof(Entry));
	if (!snapshot)
		return NULL;

	while (atomic_load(&g_running)) {
		int64_t loop_start = now_ns();
		int i, n;

		/* ① lock in → snapshot of active entries */
		registry_lock();
		Entry *entries = registry_entries(&capacity);
		for (i = 0, n = 0; i < capacity; i++)
			if (entries[i].active)
				snapshot[n++] = entries[i];
		registry_unlock();

		/* ② unlocked: probe + alert + offline writes (slow /proc /
		 * fork do not block clients). */
		for (i = 0; i < n; i++) {
			Entry *e = &snapshot[i];
			int64_t now = now_ns();

			/* §8.4: client_fd == -1 = process crashed. */
			if (e->client_fd == -1) {
				handle_process_crash(e, now);
				continue;
			}

			/* thread-alive check always runs */
			int alive = probe_thread_alive(e->pid, e->tid);
			if (alive == 0) {
				handle_exit(e, now);
				continue;
			}
			if (alive < 0)
				continue;

			/* heartbeat timeout → probe_and_detect */
			if (e->gap_ms > 0) {
				int64_t elapsed = now - e->last_heartbeat_ns;
				if (elapsed > e->gap_ms * 1000000LL) {
					probe_and_detect(e, now);
				} else if (e->alerted_deadlock ||
					   e->alerted_lock_wait ||
					   e->lock_wait_start_ns != 0) {
					/* heartbeat recovered → reset so a
					 * fresh episode alerts again */
					e->alerted_deadlock   = false;
					e->alerted_lock_wait  = false;
					e->lock_wait_start_ns = 0;
				}
			}
		}

		/* ③ lock in → apply state changes, guarded by pid/tid match
		 * (the entry may have been removed or reused concurrently). */
		registry_lock();
		entries = registry_entries(&capacity);
		for (i = 0; i < n; i++) {
			Entry *snap = &snapshot[i];
			int j;

			for (j = 0; j < capacity; j++) {
				Entry *e = &entries[j];
				if (!e->active ||
				    e->pid != snap->pid ||
				    e->tid != snap->tid)
					continue;

				e->alerted_deadlock   = snap->alerted_deadlock;
				e->alerted_lock_wait  = snap->alerted_lock_wait;
				e->lock_wait_start_ns = snap->lock_wait_start_ns;

				if (snap->alerted_exit) {
					e->alerted_exit = true;
					e->active       = false;
					e->client_fd    = -1;
				}
				break;
			}
		}
		registry_unlock();

		/* §8.7: drop stale offline-table rows. */
		registry_offline_expire(now_ns(), g_offline_ttl_ns);

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

	free(snapshot);
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
