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
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file taskhealth_mutex.h
 * @brief Companion mutex with lock-name resolution for TaskHealth.
 *
 * Wraps pthread_mutex_t with a human-readable name.  When the TaskHealth
 * watchdog detects a thread blocked on a futex, it calls
 * taskhealth_mutex_resolve() to map the futex address back to a name.
 *
 * Linking this module is optional – without it, lock names in alert
 * callbacks will show as "(unknown)".
 */

#ifndef TASKHEALTH_MUTEX_H
#define TASKHEALTH_MUTEX_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of registered mutexes. */
#define TASKHEALTH_MAX_MUTEXES 256

/**
 * @brief A named mutex tracked by the TaskHealth watchdog.
 *
 * Initialize with taskhealth_mutex_init() before use.
 */
struct taskhealth_mutex {
	pthread_mutex_t mutex;     /**< Underlying POSIX mutex. */
	const char	*name;     /**< Human-readable name for diagnostics. */
	uintptr_t	addr;      /**< Address of the embedded mutex (set by init). */
	bool		registered; /**< True if registered in the global table. */
};

/**
 * @brief Initialize a mutex and register it for name resolution.
 * @param m     Pointer to the mutex.
 * @param name  Human-readable name (copied by reference, not strdup'd).
 */
void taskhealth_mutex_init(struct taskhealth_mutex *m, const char *name);

/**
 * @brief Unregister and destroy a mutex.
 * @param m  Pointer to the mutex.
 */
void taskhealth_mutex_destroy(struct taskhealth_mutex *m);

/**
 * @brief Lock the mutex (delegates to pthread_mutex_lock).
 * @param m  Pointer to the mutex.
 */
void taskhealth_mutex_lock(struct taskhealth_mutex *m);

/**
 * @brief Unlock the mutex (delegates to pthread_mutex_unlock).
 * @param m  Pointer to the mutex.
 */
void taskhealth_mutex_unlock(struct taskhealth_mutex *m);

/**
 * @brief Resolve a futex address to a registered mutex name.
 * @param futex_addr  Virtual address of the futex.
 * @return Mutex name, or NULL if no match.
 *
 * Called by the watchdog when a thread is blocked on a futex.
 * The returned pointer is valid until taskhealth_mutex_destroy() is called.
 */
const char *taskhealth_mutex_resolve(uintptr_t futex_addr);

#ifdef __cplusplus
}
#endif

#endif
