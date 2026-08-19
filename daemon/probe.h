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

/**
 * @file probe.h
 * @brief /proc probe layer for cross-process thread inspection.
 */

#ifndef TASKHEALTH_PROBE_H
#define TASKHEALTH_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* 1 = alive, 0 = ESRCH, -1 = error  */
int         probe_thread_alive(pid_t pid, pid_t tid);

/* '?' on error */
char        probe_thread_state(pid_t pid, pid_t tid);

/* returns strlen written, 0 on error */
int         probe_wchan(pid_t pid, pid_t tid, char *buf, size_t len);

/* 0 on failure */
uintptr_t   probe_futex_addr(pid_t pid, pid_t tid);

/* caller frees; NULL on failure. format: "path+0xoffset" */
char       *probe_resolve_futex(pid_t pid, uintptr_t addr);

#endif
