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
 * @file alert.h
 * @brief Alert module — syslog, stderr, external script.
 */

#ifndef TASKHEALTH_ALERT_H
#define TASKHEALTH_ALERT_H

#include "registry.h"

#include <stdint.h>

enum alert_type {
	ALERT_EXIT,
	ALERT_DEADLOCK,
	ALERT_LOCK_WAIT,
};

int  alert_init(const char *script_path);
void alert_emit(enum alert_type type, const Entry *e,
		const char *wchan, uintptr_t futex_addr,
		int64_t wait_ms, const char *futex_module);

#endif
