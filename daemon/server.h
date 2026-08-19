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
 * @file server.h
 * @brief Unix domain socket server — accept loop + message dispatch.
 */

#ifndef TASKHEALTH_SERVER_H
#define TASKHEALTH_SERVER_H

#include <stdbool.h>

int  server_init(const char *socket_path);
void server_run(void);
void server_stop(void);
void server_request_stop(void);   /* async-signal-safe: just sets flag */

#endif
