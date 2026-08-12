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

/**
 * @file registry.h
 * @brief Thread registry for taskhealthd — data structures and API.
 */

#ifndef TASKHEALTH_REGISTRY_H
#define TASKHEALTH_REGISTRY_H

#include "../src/protocol.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define TASKHEALTH_MAX_ENTRIES  4096

typedef struct {
	pid_t    pid;                    /* client process PID */
	pid_t    tid;                    /* kernel thread TID */
	int      client_fd;              /* client socket fd, -1 = free slot */
	char     name[TASKHEALTH_NAME_LEN];
	int64_t  gap_ms;
	int64_t  lock_hold_ms;

	int64_t  last_heartbeat_ns;      /* updated on MSG_HEARTBEAT, CLOCK_MONOTONIC */
	int64_t  lock_wait_start_ns;     /* first time we saw it on a futex this episode */
	bool     active;
	bool     alerted_exit;
	bool     alerted_deadlock;
	bool     alerted_lock_wait;
} Entry;

int  registry_init(int capacity);
void registry_destroy(void);

Entry *registry_add(const msg_body_register_t *body, int client_fd);
int    registry_heartbeat(pid_t pid, pid_t tid, int64_t now_ns);
int    registry_remove(pid_t pid, pid_t tid);
void   registry_cleanup_pid(pid_t pid);
void   registry_cleanup_fd(int fd);

/* for watchdog: return array + count (caller holds lock during scan) */
Entry *registry_entries(int *capacity);
void   registry_lock(void);
void   registry_unlock(void);

#endif
