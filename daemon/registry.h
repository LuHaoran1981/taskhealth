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

/**
 * @file registry.h
 * @brief Thread registry for taskhealthd — data structures and API.
 */

#ifndef TASKHEALTH_REGISTRY_H
#define TASKHEALTH_REGISTRY_H

#include "taskhealth/protocol.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define TASKHEALTH_MAX_ENTRIES  4096

/* Total lock-name entries across all clients (daemon-side mirror of the
 * client's per-process TASKHEALTH_MAX_MUTEXES table). */
#define TASKHEALTH_MAX_MUTEXES  8192

typedef struct {
	pid_t    pid;                    /* client process PID */
	pid_t    tid;                    /* kernel thread TID */
	int      client_fd;              /* client socket fd, -1 = free slot */
	char     name[TASKHEALTH_NAME_LEN];
	int64_t  gap_ms;
	int64_t  lock_hold_ms;

	int64_t  last_heartbeat_ns;      /* updated on MSG_HEARTBEAT, CLOCK_MONOTONIC */
	int64_t  lock_wait_start_ns;     /* first time we saw it on a futex this episode */
	uint64_t wait_futex_addr;        /* cooperative futex addr reported before blocking */
	bool     active;
	bool     alerted_exit;
	bool     alerted_deadlock;
	bool     alerted_lock_wait;
} Entry;

/* Result of a registry_add attempt. */
typedef enum {
	REGISTRY_ADD_OK = 0,      /* entry added, *out points to it */
	REGISTRY_ADD_DUPLICATE,   /* same (pid, tid) already active */
	REGISTRY_ADD_FULL,        /* no free slot */
} registry_add_result_t;

int  registry_init(int capacity);
void registry_destroy(void);

registry_add_result_t registry_add(const msg_body_register_t *body,
				    int client_fd, Entry **out);
int    registry_heartbeat(pid_t pid, pid_t tid, int64_t now_ns);
int    registry_remove(pid_t pid, pid_t tid);
void   registry_lock_wait(pid_t pid, pid_t tid, uint64_t futex_addr);
void   registry_lock_acquired(pid_t pid, pid_t tid);
void   registry_cleanup_pid(pid_t pid);
void   registry_cleanup_fd(int fd);

/* ── lock-name table (keyed by client_fd + futex_addr) ─────────────── */

void        registry_mutex_add(int client_fd, uint64_t futex_addr,
			       const char *name);
void        registry_mutex_remove(int client_fd, uint64_t futex_addr);
/* Returns 1 on match and copies the name into name_out, else 0. */
int         registry_mutex_resolve(int client_fd, uint64_t futex_addr,
				   char *name_out, size_t name_len);
void        registry_mutex_cleanup_fd(int client_fd);

/* for watchdog: return array + count (caller holds lock during scan) */
Entry *registry_entries(int *capacity);
void   registry_lock(void);
void   registry_unlock(void);

#endif
