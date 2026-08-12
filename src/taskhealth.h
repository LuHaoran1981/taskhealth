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
 * @file taskhealth.h
 * @brief Linux user-space thread health monitoring — client library.
 *
 * The TaskHealth client library connects to the taskhealthd daemon over a
 * Unix domain socket and sends registration, heartbeat, and unregistration
 * messages.  The daemon (taskhealthd) performs cross-process thread
 * health monitoring via /proc probes.
 *
 * Usage:
 *
 *   1. taskhealth_init(NULL)                        — connect to daemon
 *   2. taskhealth_register("mythread", 3000, 0)      — register current thread
 *   3. taskhealth_heartbeat()   (in worker loop)       — send heartbeat
 *   4. taskhealth_unregister()                        — clean unregister
 *   5. taskhealth_shutdown()                          — disconnect, cleanup
 *
 * All detection and alerting happens in the daemon — the client library
 * only handles IPC messaging.
 */

#ifndef TASKHEALTH_H
#define TASKHEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TASKHEALTH_VERSION
#define TASKHEALTH_VERSION "0.0.0"
#endif

/** Maximum length of a registered thread name. */
#define TASKHEALTH_NAME_LEN  32

/* ── error codes ────────────────────────────────────────────────────── */

#define TASKHEALTH_OK                0
#define TASKHEALTH_ERR_NOT_INIT     -1
#define TASKHEALTH_ERR_ALREADY_INIT -2
#define TASKHEALTH_ERR_TABLE_FULL   -3
#define TASKHEALTH_ERR_ALREADY_REG  -4
#define TASKHEALTH_ERR_NOT_REG      -5
#define TASKHEALTH_ERR_INVALID_ARG  -6

/* ── configuration ──────────────────────────────────────────────────── */

/**
 * @brief Client library configuration.
 *
 * All fields may be zero/NULL to accept defaults.  The client library
 * only cares about socket_path — watchdog parameters are configured
 * in the daemon.
 */
struct taskhealth_config {
	/** Daemon socket path, empty = default "/run/taskhealth.sock". */
	char socket_path[256];
};

/* ── API ────────────────────────────────────────────────────────────── */

/** @brief Return the library version string (e.g. "0.1.0"). */
const char *taskhealth_version(void);

/**
 * @brief Connect to the taskhealthd daemon.
 * @param cfg  Configuration, or NULL for defaults.
 * @return TASKHEALTH_OK on success, negative error code on failure.
 */
int  taskhealth_init(const struct taskhealth_config *cfg);

/**
 * @brief Disconnect from the daemon and release resources.
 *
 * Sends MSG_SHUTDOWN so the daemon cleans up all entries for this process.
 * Safe to call multiple times.
 */
void taskhealth_shutdown(void);

/**
 * @brief Register the calling thread for monitoring.
 *
 * @param name          Thread name for alert messages.
 *                      NULL or ""  → auto-generate as "exename.TID".
 * @param gap_ms        Heartbeat timeout in ms (0 = exit detection only).
 * @param lock_hold_ms  Lock-wait threshold in ms (0 = disabled).
 * @return TASKHEALTH_OK on success, negative error code on failure.
 */
int  taskhealth_register(const char *name, int64_t gap_ms, int64_t lock_hold_ms);

/**
 * @brief Unregister the calling thread.
 *
 * Cleanly unregistered threads do not trigger an exit alert.
 * Safe to call even if not registered (no-op).
 */
void taskhealth_unregister(void);

/**
 * @brief Send a heartbeat to the daemon.
 *
 * Call periodically from the worker loop.  No effect if the thread
 * is not registered or gap_ms is 0.
 */
void taskhealth_heartbeat(void);

#ifdef __cplusplus
}
#endif

#endif
