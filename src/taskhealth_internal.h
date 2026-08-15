/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <luhaoran@symthosm.com>
 *         芦浩然
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file taskhealth_internal.h
 * @brief Internal client hooks shared between taskhealth.c and
 *        taskhealth_mutex.c.  Not part of the public API.
 */

#ifndef TASKHEALTH_INTERNAL_H
#define TASKHEALTH_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Send MSG_MUTEX_REGISTER / MSG_MUTEX_UNREGISTER if the client is connected.
 * No-op when the client has not been initialized. */
void taskhealth_mutex_notify_register(uintptr_t futex_addr, const char *name);
void taskhealth_mutex_notify_unregister(uintptr_t futex_addr);

/* Report the calling thread is about to block on / has acquired the futex at
 * futex_addr.  No-op when the client has not been initialized. */
void taskhealth_mutex_notify_lock_wait(uintptr_t futex_addr);
void taskhealth_mutex_notify_lock_acquired(uintptr_t futex_addr);

#ifdef __cplusplus
}
#endif

#endif
