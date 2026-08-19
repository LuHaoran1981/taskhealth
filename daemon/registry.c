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
#include "registry.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct {
	Entry          *entries;
	int             capacity;
	pthread_mutex_t lock;
} g_reg;

typedef struct {
	int      client_fd;   /* -1 = free slot */
	uint64_t futex_addr;
	char     name[TASKHEALTH_NAME_LEN];
} MutexEntry;

static struct {
	MutexEntry     *entries;
	int             capacity;
	pthread_mutex_t lock;
} g_mutex;

/* ── offline / recovery tables ─────────────────────────────────────── */

static struct {
	OfflineThread   *entries;
	int              capacity;
	pthread_mutex_t  lock;
} g_off_t;

static struct {
	OfflineProcess  *entries;
	int              capacity;
	pthread_mutex_t  lock;
} g_off_p;

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void read_proc_comm(pid_t pid, char *out, size_t len)
{
	char path[64];
	FILE *f;
	char *nl;

	if (!out || len == 0) return;
	out[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	f = fopen(path, "r");
	if (!f) return;
	if (fgets(out, (int)len, f)) {
		nl = strchr(out, '\n');
		if (nl) *nl = '\0';
	}
	fclose(f);
}

/* §8.6: distinguish same-name threads within one process by appending a
 * "#N" suffix.  Caller must hold g_reg.lock. */
static void disambiguate_name(pid_t pid, const char *base,
			      char *out, size_t out_len)
{
	int seq;

	for (seq = 0; seq < 10000; seq++) {
		int i;
		bool collision = false;

		if (seq == 0) {
			snprintf(out, out_len, "%s", base);
		} else {
			/* truncate the base so the "#N" suffix always fits */
			char suffix[16];
			size_t n, room, slen;
			snprintf(suffix, sizeof(suffix), "#%d", seq + 1);
			slen = strlen(suffix);
			n    = strlen(base);
			room = out_len - slen - 1;
			if (n > room)
				n = room;
			memcpy(out, base, n);
			memcpy(out + n, suffix, slen + 1);
		}

		for (i = 0; i < g_reg.capacity; i++) {
			if (g_reg.entries[i].active &&
			    g_reg.entries[i].pid == pid &&
			    strcmp(g_reg.entries[i].name, out) == 0) {
				collision = true;
				break;
			}
		}
		if (!collision)
			return;
	}
	/* pathological: leave the base name as-is */
	snprintf(out, out_len, "%s", base);
}

/* ── init / destroy ─────────────────────────────────────────────────── */

int registry_init(int capacity)
{
	int i;

	memset(&g_reg, 0, sizeof(g_reg));
	g_reg.entries = calloc((size_t)capacity, sizeof(Entry));
	if (!g_reg.entries) return -1;
	g_reg.capacity = capacity;
	pthread_mutex_init(&g_reg.lock, NULL);

	g_mutex.entries = calloc(TASKHEALTH_MAX_MUTEXES, sizeof(MutexEntry));
	if (!g_mutex.entries) {
		free(g_reg.entries);
		g_reg.entries = NULL;
		g_reg.capacity = 0;
		pthread_mutex_destroy(&g_reg.lock);
		return -1;
	}
	g_mutex.capacity = TASKHEALTH_MAX_MUTEXES;
	for (i = 0; i < g_mutex.capacity; i++)
		g_mutex.entries[i].client_fd = -1;
	pthread_mutex_init(&g_mutex.lock, NULL);

	g_off_t.entries = calloc(TASKHEALTH_MAX_OFFLINE_THREADS,
				 sizeof(OfflineThread));
	if (!g_off_t.entries) {
		free(g_mutex.entries);
		g_mutex.entries = NULL;
		g_mutex.capacity = 0;
		pthread_mutex_destroy(&g_mutex.lock);
		free(g_reg.entries);
		g_reg.entries = NULL;
		g_reg.capacity = 0;
		pthread_mutex_destroy(&g_reg.lock);
		return -1;
	}
	g_off_t.capacity = TASKHEALTH_MAX_OFFLINE_THREADS;
	pthread_mutex_init(&g_off_t.lock, NULL);

	g_off_p.entries = calloc(TASKHEALTH_MAX_OFFLINE_THREADS,
				 sizeof(OfflineProcess));
	if (!g_off_p.entries) {
		free(g_off_t.entries);
		g_off_t.entries = NULL;
		g_off_t.capacity = 0;
		pthread_mutex_destroy(&g_off_t.lock);
		free(g_mutex.entries);
		g_mutex.entries = NULL;
		g_mutex.capacity = 0;
		pthread_mutex_destroy(&g_mutex.lock);
		free(g_reg.entries);
		g_reg.entries = NULL;
		g_reg.capacity = 0;
		pthread_mutex_destroy(&g_reg.lock);
		return -1;
	}
	g_off_p.capacity = TASKHEALTH_MAX_OFFLINE_THREADS;
	pthread_mutex_init(&g_off_p.lock, NULL);

	return 0;
}

void registry_destroy(void)
{
	free(g_reg.entries);
	g_reg.entries = NULL;
	g_reg.capacity = 0;
	pthread_mutex_destroy(&g_reg.lock);

	free(g_mutex.entries);
	g_mutex.entries = NULL;
	g_mutex.capacity = 0;
	pthread_mutex_destroy(&g_mutex.lock);

	free(g_off_t.entries);
	g_off_t.entries = NULL;
	g_off_t.capacity = 0;
	pthread_mutex_destroy(&g_off_t.lock);

	free(g_off_p.entries);
	g_off_p.entries = NULL;
	g_off_p.capacity = 0;
	pthread_mutex_destroy(&g_off_p.lock);
}

/* ── offline helpers (lock internally) ─────────────────────────────── */

void registry_offline_thread_add(const char *proc_name, const char *name,
				 pid_t pid, pid_t tid, int64_t now)
{
	int i;
	int free_slot = -1;

	if (!proc_name || !name) return;

	pthread_mutex_lock(&g_off_t.lock);
	for (i = 0; i < g_off_t.capacity; i++) {
		OfflineThread *o = &g_off_t.entries[i];
		if (!o->used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(o->proc_name, proc_name) == 0 &&
		    strcmp(o->name, name) == 0) {
			/* same key: refresh instead of appending a duplicate */
			o->pid           = pid;
			o->tid           = tid;
			o->offline_at_ns = now;
			pthread_mutex_unlock(&g_off_t.lock);
			return;
		}
	}
	if (free_slot >= 0) {
		OfflineThread *o = &g_off_t.entries[free_slot];
		snprintf(o->proc_name, sizeof(o->proc_name), "%s", proc_name);
		snprintf(o->name, sizeof(o->name), "%s", name);
		o->pid           = pid;
		o->tid           = tid;
		o->offline_at_ns = now;
		o->used          = true;
	}
	pthread_mutex_unlock(&g_off_t.lock);
}

void registry_offline_process_add(const char *proc_name, pid_t pid,
				  int64_t now)
{
	int i;
	int free_slot = -1;

	if (!proc_name) return;

	pthread_mutex_lock(&g_off_p.lock);
	for (i = 0; i < g_off_p.capacity; i++) {
		OfflineProcess *o = &g_off_p.entries[i];
		if (!o->used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(o->proc_name, proc_name) == 0) {
			/* one crashed process has many threads: collapse to a
			 * single offline-process row keyed by proc_name */
			o->pid           = pid;
			o->offline_at_ns = now;
			pthread_mutex_unlock(&g_off_p.lock);
			return;
		}
	}
	if (free_slot >= 0) {
		OfflineProcess *o = &g_off_p.entries[free_slot];
		snprintf(o->proc_name, sizeof(o->proc_name), "%s", proc_name);
		o->pid           = pid;
		o->offline_at_ns = now;
		o->used          = true;
	}
	pthread_mutex_unlock(&g_off_p.lock);
}

void registry_offline_thread_remove(const char *proc_name, const char *name)
{
	int i;

	if (!proc_name || !name) return;

	pthread_mutex_lock(&g_off_t.lock);
	for (i = 0; i < g_off_t.capacity; i++) {
		if (g_off_t.entries[i].used &&
		    strcmp(g_off_t.entries[i].proc_name, proc_name) == 0 &&
		    strcmp(g_off_t.entries[i].name, name) == 0) {
			g_off_t.entries[i].used = false;
			break;
		}
	}
	pthread_mutex_unlock(&g_off_t.lock);
}

void registry_offline_expire(int64_t now, int64_t ttl_ns)
{
	int i;

	pthread_mutex_lock(&g_off_t.lock);
	for (i = 0; i < g_off_t.capacity; i++) {
		if (g_off_t.entries[i].used &&
		    now - g_off_t.entries[i].offline_at_ns > ttl_ns)
			g_off_t.entries[i].used = false;
	}
	pthread_mutex_unlock(&g_off_t.lock);

	pthread_mutex_lock(&g_off_p.lock);
	for (i = 0; i < g_off_p.capacity; i++) {
		if (g_off_p.entries[i].used &&
		    now - g_off_p.entries[i].offline_at_ns > ttl_ns)
			g_off_p.entries[i].used = false;
	}
	pthread_mutex_unlock(&g_off_p.lock);
}

int registry_offline_threads(OfflineThread **arr, int *cap)
{
	*arr = g_off_t.entries;
	*cap = g_off_t.capacity;
	return 0;
}

int registry_offline_processes(OfflineProcess **arr, int *cap)
{
	*arr = g_off_p.entries;
	*cap = g_off_p.capacity;
	return 0;
}

/* ── core registry ops ─────────────────────────────────────────────── */

registry_add_result_t registry_add(const msg_body_register_t *body,
				   int client_fd, const char *proc_name,
				   Entry **out,
				   registry_recovery_info_t *recovery)
{
	char comm_buf[TASKHEALTH_NAME_LEN];
	char final_name[TASKHEALTH_NAME_LEN];
	bool recovered_thread  = false;
	bool recovered_process = false;
	pid_t old_tid = 0, old_pid = 0;
	int i;

	if (recovery) memset(recovery, 0, sizeof(*recovery));

	/* resolve /proc/<pid>/comm if caller didn't supply it */
	if (proc_name && proc_name[0]) {
		snprintf(comm_buf, sizeof(comm_buf), "%s", proc_name);
	} else {
		read_proc_comm((pid_t)body->pid, comm_buf, sizeof(comm_buf));
	}
	if (comm_buf[0] == '\0')
		snprintf(comm_buf, sizeof(comm_buf), "pid.%d", (int)body->pid);

	/* ── duplicate check + same-name disambiguation (§8.6) ──────── */
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		if (g_reg.entries[i].active &&
		    g_reg.entries[i].pid == body->pid &&
		    g_reg.entries[i].tid == body->tid) {
			pthread_mutex_unlock(&g_reg.lock);
			return REGISTRY_ADD_DUPLICATE;
		}
	}
	disambiguate_name((pid_t)body->pid, body->name,
			  final_name, sizeof(final_name));
	pthread_mutex_unlock(&g_reg.lock);

	/* ── recovery lookup (§8.5) ──────────────────────────────────── */

	/* (1) offline-thread table: key = (proc_name, name).  Counts as a
	 * recovery only if the old process is gone; otherwise it is a
	 * same-name thread re-registering inside a still-live process. */
	pthread_mutex_lock(&g_off_t.lock);
	for (i = 0; i < g_off_t.capacity; i++) {
		OfflineThread *o = &g_off_t.entries[i];
		if (!o->used ||
		    strcmp(o->proc_name, comm_buf) != 0 ||
		    strcmp(o->name, final_name) != 0)
			continue;

		if (kill(o->pid, 0) < 0 && errno == ESRCH) {
			recovered_thread = true;
			old_tid          = o->tid;
			old_pid          = o->pid;
		}
		o->used = false;   /* consumed either way */
		break;
	}
	pthread_mutex_unlock(&g_off_t.lock);

	/* (2) fast-restart race: old entry is still active but its socket
	 * already hung up (client_fd == -1) and the watchdog has not swept
	 * it into the offline tables yet. */
	if (!recovered_thread) {
		pthread_mutex_lock(&g_reg.lock);
		for (i = 0; i < g_reg.capacity; i++) {
			Entry *e = &g_reg.entries[i];
			if (e->active && e->client_fd == -1 &&
			    strcmp(e->proc_name, comm_buf) == 0 &&
			    strcmp(e->name, final_name) == 0) {
				recovered_thread = true;
				old_tid          = e->tid;
				old_pid          = e->pid;
				e->active        = false;
				break;
			}
		}
		pthread_mutex_unlock(&g_reg.lock);
	}

	/* (3) offline-process table: key = proc_name. */
	pthread_mutex_lock(&g_off_p.lock);
	for (i = 0; i < g_off_p.capacity; i++) {
		OfflineProcess *o = &g_off_p.entries[i];
		if (!o->used || strcmp(o->proc_name, comm_buf) != 0)
			continue;

		if (kill(o->pid, 0) < 0 && errno == ESRCH) {
			recovered_process = true;
			old_pid           = o->pid;
		}
		o->used = false;   /* consumed either way */
		break;
	}
	pthread_mutex_unlock(&g_off_p.lock);

	/* ── insert ─────────────────────────────────────────────────── */
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		if (!g_reg.entries[i].active) {
			Entry *e = &g_reg.entries[i];
			memset(e, 0, sizeof(*e));
			e->pid       = (pid_t)body->pid;
			e->tid       = (pid_t)body->tid;
			e->client_fd = client_fd;
			snprintf(e->proc_name, sizeof(e->proc_name), "%s",
				 comm_buf);
			snprintf(e->name, sizeof(e->name), "%s", final_name);
			e->gap_ms       = body->gap_ms;
			e->lock_hold_ms = body->lock_hold_ms;
			e->active       = true;
			/* registration counts as the first heartbeat timestamp */
			e->last_heartbeat_ns = now_ns();
			if (out)
				*out = e;
			pthread_mutex_unlock(&g_reg.lock);

			if (recovery) {
				recovery->recovered_thread  = recovered_thread;
				recovery->recovered_process = recovered_process;
				recovery->old_tid           = old_tid;
				recovery->new_tid           = e->tid;
				recovery->old_pid           = old_pid;
				recovery->new_pid           = e->pid;
			}
			return (recovered_thread || recovered_process) ?
				REGISTRY_ADD_RECOVERED : REGISTRY_ADD_OK;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
	return REGISTRY_ADD_FULL;
}

int registry_heartbeat(pid_t pid, pid_t tid, int64_t now_ns)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->last_heartbeat_ns = now_ns;
			pthread_mutex_unlock(&g_reg.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
	return -1;
}

void registry_lock_wait(pid_t pid, pid_t tid, uint64_t futex_addr)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->wait_futex_addr = futex_addr;
			pthread_mutex_unlock(&g_reg.lock);
			return;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
}

void registry_lock_acquired(pid_t pid, pid_t tid)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->wait_futex_addr = 0;
			pthread_mutex_unlock(&g_reg.lock);
			return;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
}

int registry_remove(pid_t pid, pid_t tid)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->active    = false;
			e->client_fd = -1;
			pthread_mutex_unlock(&g_reg.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
	return -1;
}

/* On socket HUP: keep active=true, only clear client_fd so the watchdog
 * can later determine this was a process crash (vs. a clean unregister). */
void registry_disconnect_fd(int fd)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->client_fd == fd)
			e->client_fd = -1;
	}
	pthread_mutex_unlock(&g_reg.lock);
}

void registry_cleanup_pid(pid_t pid)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid) {
			e->active    = false;
			e->client_fd = -1;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
}

/* Hard remove (used on MSG_SHUTDOWN). */
void registry_cleanup_fd(int fd)
{
	int i;
	pthread_mutex_lock(&g_reg.lock);
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->client_fd == fd) {
			e->active    = false;
			e->client_fd = -1;
		}
	}
	pthread_mutex_unlock(&g_reg.lock);
}

Entry *registry_entries(int *capacity)
{
	*capacity = g_reg.capacity;
	return g_reg.entries;
}

void registry_lock(void)
{
	pthread_mutex_lock(&g_reg.lock);
}

void registry_unlock(void)
{
	pthread_mutex_unlock(&g_reg.lock);
}

/* ── lock-name table ────────────────────────────────────────────────── */

void registry_mutex_add(int client_fd, uint64_t futex_addr, const char *name)
{
	int i;

	pthread_mutex_lock(&g_mutex.lock);

	for (i = 0; i < g_mutex.capacity; i++) {
		if (g_mutex.entries[i].client_fd == client_fd &&
		    g_mutex.entries[i].futex_addr == futex_addr) {
			snprintf(g_mutex.entries[i].name,
				 sizeof(g_mutex.entries[i].name), "%s",
				 name ? name : "");
			pthread_mutex_unlock(&g_mutex.lock);
			return;
		}
	}

	for (i = 0; i < g_mutex.capacity; i++) {
		if (g_mutex.entries[i].client_fd == -1) {
			g_mutex.entries[i].client_fd  = client_fd;
			g_mutex.entries[i].futex_addr = futex_addr;
			snprintf(g_mutex.entries[i].name,
				 sizeof(g_mutex.entries[i].name), "%s",
				 name ? name : "");
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex.lock);
}

void registry_mutex_remove(int client_fd, uint64_t futex_addr)
{
	int i;

	pthread_mutex_lock(&g_mutex.lock);
	for (i = 0; i < g_mutex.capacity; i++) {
		if (g_mutex.entries[i].client_fd == client_fd &&
		    g_mutex.entries[i].futex_addr == futex_addr) {
			g_mutex.entries[i].client_fd = -1;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex.lock);
}

int registry_mutex_resolve(int client_fd, uint64_t futex_addr,
			   char *name_out, size_t name_len)
{
	int i;
	int found = 0;

	if (!name_out || name_len == 0)
		return 0;

	pthread_mutex_lock(&g_mutex.lock);
	for (i = 0; i < g_mutex.capacity; i++) {
		if (g_mutex.entries[i].client_fd == client_fd &&
		    g_mutex.entries[i].futex_addr == futex_addr) {
			snprintf(name_out, name_len, "%s",
				 g_mutex.entries[i].name);
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex.lock);
	return found;
}

void registry_mutex_cleanup_fd(int client_fd)
{
	int i;

	pthread_mutex_lock(&g_mutex.lock);
	for (i = 0; i < g_mutex.capacity; i++) {
		if (g_mutex.entries[i].client_fd == client_fd)
			g_mutex.entries[i].client_fd = -1;
	}
	pthread_mutex_unlock(&g_mutex.lock);
}
