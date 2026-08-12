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
 * @file watchdog.h
 * @brief Watchdog thread API for taskhealthd.
 */

#ifndef TASKHEALTH_WATCHDOG_H
#define TASKHEALTH_WATCHDOG_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

int  watchdog_start(int64_t check_interval_ms);
void watchdog_stop(void);
bool watchdog_running(void);

#endif
