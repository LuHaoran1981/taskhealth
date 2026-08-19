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
 * @file alert.h
 * @brief Alert module — syslog, log file, stderr, external script.
 */

#ifndef TASKHEALTH_ALERT_H
#define TASKHEALTH_ALERT_H

#include "registry.h"

#include <stdint.h>

enum alert_type {
	ALERT_EXIT,
	ALERT_DEADLOCK,
	ALERT_LOCK_WAIT,
	ALERT_RECOVERY,   /* §8.8: NORMAL severity (syslog + log only) */
};

/**
 * @brief Alert severity.
 *
 * CRITICAL alerts are printed to stderr AND recorded to the log file
 * and may invoke the external script; NORMAL alerts are only recorded
 * to the log file (no stderr noise, no script).
 */
enum alert_severity {
	SEV_NORMAL,
	SEV_CRITICAL,
};

/**
 * @brief Initialize the alert module.
 * @param script_path  Optional external alert script (NULL/"" = none).
 * @param log_file     Optional log file path (NULL/"" = syslog only).
 */
int  alert_init(const char *script_path, const char *log_file);

/* §3.6 alert_emit — for EXIT / DEADLOCK / LOCK_WAIT. */
void alert_emit(enum alert_type type, const Entry *e,
		const char *wchan, uintptr_t futex_addr,
		int64_t wait_ms, const char *futex_module,
		const char *lock_name);

/* §8.8 recovery alert (NORMAL severity).  kind = "process" or "thread". */
void alert_emit_recovery(const char *kind,
			 const registry_recovery_info_t *r);

#endif
