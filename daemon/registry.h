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
	char     proc_name[TASKHEALTH_NAME_LEN]; /* /proc/<pid>/comm at register */
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
	REGISTRY_ADD_OK = 0,           /* entry added, *out points to it */
	REGISTRY_ADD_DUPLICATE,        /* same (pid, tid) already active */
	REGISTRY_ADD_FULL,             /* no free slot */
	REGISTRY_ADD_RECOVERED,        /* added, and matched an offline entry */
} registry_add_result_t;

/* Populated by registry_add when it returns REGISTRY_ADD_OK /
 * REGISTRY_ADD_RECOVERED.  Lets the caller (server.c) emit a RECOVERY
 * alert without re-scanning the offline tables. */
typedef struct {
	bool     recovered_thread;      /* offline-thread table hit (proc, name) */
	bool     recovered_process;     /* process-name restart detected */
	pid_t    old_tid;               /* previous tid (or 0 if new) */
	pid_t    new_tid;               /* current tid */
	pid_t    old_pid;               /* previous pid (or 0 if new) */
	pid_t    new_pid;               /* current pid */
} registry_recovery_info_t;

int  registry_init(int capacity);
void registry_destroy(void);

/* proc_name: process comm from /proc/<pid>/comm; if NULL/empty, registry_add
 * will read it itself.  On success (REGISTRY_ADD_OK / REGISTRY_ADD_RECOVERED)
 * *out points to the inserted entry and *recovery (if non-NULL) is filled. */
registry_add_result_t registry_add(const msg_body_register_t *body,
				   int client_fd, const char *proc_name,
				   Entry **out,
				   registry_recovery_info_t *recovery);
int    registry_heartbeat(pid_t pid, pid_t tid, int64_t now_ns);
int    registry_remove(pid_t pid, pid_t tid);
void   registry_lock_wait(pid_t pid, pid_t tid, uint64_t futex_addr);
void   registry_lock_acquired(pid_t pid, pid_t tid);
/* On socket hangup: clear client_fd but keep active=true so the watchdog
 * can attribute a subsequent thread-not-found to a process crash. */
void   registry_disconnect_fd(int fd);
void   registry_cleanup_pid(pid_t pid);
/* Hard remove (used on MSG_SHUTDOWN). */
void   registry_cleanup_fd(int fd);

/* ── offline / recovery tables (§8) ────────────────────────────────── */

#define TASKHEALTH_MAX_OFFLINE_THREADS  TASKHEALTH_MAX_ENTRIES
#define TASKHEALTH_OFFLINE_TTL_NS       (2LL * 60LL * 60LL * 1000000000LL)

typedef struct {
	bool     used;
	char     proc_name[TASKHEALTH_NAME_LEN];
	char     name[TASKHEALTH_NAME_LEN];
	int64_t  offline_at_ns;
	pid_t    pid;
	pid_t    tid;
} OfflineThread;

typedef struct {
	bool     used;
	char     proc_name[TASKHEALTH_NAME_LEN];
	int64_t  offline_at_ns;
	pid_t    pid;
} OfflineProcess;

/* Drop entries older than ttl_ns from both offline tables.  Caller does
 * not hold any registry lock. */
void   registry_offline_expire(int64_t now_ns, int64_t ttl_ns);

/* Append to offline table.  Caller does not hold any registry lock. */
void   registry_offline_thread_add(const char *proc_name, const char *name,
				   pid_t pid, pid_t tid, int64_t now_ns);
void   registry_offline_process_add(const char *proc_name, pid_t pid,
				     int64_t now_ns);

/* Drop a thread entry after a successful recovery registration. */
void   registry_offline_thread_remove(const char *proc_name, const char *name);

/* Snapshot accessors.  Returned arrays are owned by the registry and remain
 * valid for the lifetime of the daemon; caller must not free them. */
int    registry_offline_threads(OfflineThread **arr, int *cap);
int    registry_offline_processes(OfflineProcess **arr, int *cap);

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
