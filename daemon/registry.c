/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <luhaoran1981@icloud.com>
 *         芦浩然
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
}

registry_add_result_t registry_add(const msg_body_register_t *body,
				    int client_fd, Entry **out)
{
	int i;

	/* check for duplicate tid */
	for (i = 0; i < g_reg.capacity; i++) {
		if (g_reg.entries[i].active &&
		    g_reg.entries[i].pid == body->pid &&
		    g_reg.entries[i].tid == body->tid) {
			return REGISTRY_ADD_DUPLICATE;
		}
	}

	for (i = 0; i < g_reg.capacity; i++) {
		if (!g_reg.entries[i].active) {
			Entry *e = &g_reg.entries[i];
			memset(e, 0, sizeof(*e));
			e->pid       = (pid_t)body->pid;
			e->tid       = (pid_t)body->tid;
			e->client_fd = client_fd;
			snprintf(e->name, sizeof(e->name), "%s", body->name);
			e->gap_ms       = body->gap_ms;
			e->lock_hold_ms = body->lock_hold_ms;
			e->active       = true;
			*out = e;
			return REGISTRY_ADD_OK;
		}
	}
	return REGISTRY_ADD_FULL;
}

int registry_heartbeat(pid_t pid, pid_t tid, int64_t now_ns)
{
	int i;
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->last_heartbeat_ns = now_ns;
			return 0;
		}
	}
	return -1;
}

void registry_lock_wait(pid_t pid, pid_t tid, uint64_t futex_addr)
{
	int i;
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->wait_futex_addr = futex_addr;
			return;
		}
	}
}

void registry_lock_acquired(pid_t pid, pid_t tid)
{
	int i;
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->wait_futex_addr = 0;
			return;
		}
	}
}

int registry_remove(pid_t pid, pid_t tid)
{
	int i;
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid && e->tid == tid) {
			e->active = false;
			e->client_fd = -1;
			return 0;
		}
	}
	return -1;
}

void registry_cleanup_pid(pid_t pid)
{
	int i;
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->pid == pid) {
			e->active = false;
			e->client_fd = -1;
		}
	}
}

void registry_cleanup_fd(int fd)
{
	int i;
	for (i = 0; i < g_reg.capacity; i++) {
		Entry *e = &g_reg.entries[i];
		if (e->active && e->client_fd == fd) {
			e->active = false;
			e->client_fd = -1;
		}
	}
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

	/* re-init of the same mutex → update the existing name */
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
