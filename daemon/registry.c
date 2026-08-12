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

#include "registry.h"

#include <stdlib.h>
#include <string.h>

static struct {
	Entry          *entries;
	int             capacity;
	pthread_mutex_t lock;
} g_reg;

int registry_init(int capacity)
{
	memset(&g_reg, 0, sizeof(g_reg));
	g_reg.entries = calloc((size_t)capacity, sizeof(Entry));
	if (!g_reg.entries) return -1;
	g_reg.capacity = capacity;
	pthread_mutex_init(&g_reg.lock, NULL);
	return 0;
}

void registry_destroy(void)
{
	free(g_reg.entries);
	g_reg.entries = NULL;
	g_reg.capacity = 0;
	pthread_mutex_destroy(&g_reg.lock);
}

Entry *registry_add(const msg_body_register_t *body, int client_fd)
{
	int i;

	/* check for duplicate tid */
	for (i = 0; i < g_reg.capacity; i++) {
		if (g_reg.entries[i].active &&
		    g_reg.entries[i].pid == body->pid &&
		    g_reg.entries[i].tid == body->tid) {
			return NULL;  /* already registered */
		}
	}

	for (i = 0; i < g_reg.capacity; i++) {
		if (!g_reg.entries[i].active) {
			Entry *e = &g_reg.entries[i];
			memset(e, 0, sizeof(*e));
			e->pid       = (pid_t)body->pid;
			e->tid       = (pid_t)body->tid;
			e->client_fd = client_fd;
			strncpy(e->name, body->name, TASKHEALTH_NAME_LEN - 1);
			e->name[TASKHEALTH_NAME_LEN - 1] = '\0';
			e->gap_ms       = body->gap_ms;
			e->lock_hold_ms = body->lock_hold_ms;
			e->active       = true;
			return e;
		}
	}
	return NULL;  /* table full */
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
