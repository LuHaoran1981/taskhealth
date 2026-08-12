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
 * @file protocol.h
 * @brief Shared IPC protocol definitions for TaskHealth.
 *
 * Included by both the client library (libtaskhealth) and the daemon
 * (taskhealthd).  All message structs are __attribute__((packed)) with
 * compile-time _Static_assert size checks.
 */

#ifndef TASKHEALTH_PROTOCOL_H
#define TASKHEALTH_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── magic ──────────────────────────────────────────────────────────── */

#define TASKHEALTH_MSG_MAGIC  0x5448  /* "TH" */

/* ── message types ──────────────────────────────────────────────────── */

enum taskhealth_msg_type {
	MSG_REGISTER   = 1,   /* client → daemon: register thread */
	MSG_UNREGISTER = 2,   /* client → daemon: unregister thread */
	MSG_HEARTBEAT  = 3,   /* client → daemon: heartbeat (one-way) */
	MSG_SHUTDOWN   = 4,   /* client → daemon: process exiting (one-way) */
	MSG_RESPONSE   = 5,   /* daemon → client: response to REGISTER */
};

/* ── header ─────────────────────────────────────────────────────────── */

typedef struct {
	uint16_t magic;       /* TASKHEALTH_MSG_MAGIC */
	uint8_t  type;        /* enum taskhealth_msg_type */
	uint8_t  _reserved;
	uint32_t body_len;    /* body length in bytes, host byte order */
} __attribute__((packed)) taskhealth_msg_hdr_t;

_Static_assert(sizeof(taskhealth_msg_hdr_t) == 8, "hdr size");

/* ── body: MSG_REGISTER ─────────────────────────────────────────────── */

#define TASKHEALTH_NAME_LEN  32

typedef struct {
	int32_t  pid;                   /*  4B  getpid() */
	int32_t  tid;                   /*  4B  gettid() */
	char     name[TASKHEALTH_NAME_LEN]; /* 32B */
	int64_t  gap_ms;                /*  8B  heartbeat timeout (ms), 0=exit only */
	int64_t  lock_hold_ms;          /*  8B  lock-wait threshold (ms), 0=disabled */
} __attribute__((packed)) msg_body_register_t;

_Static_assert(sizeof(msg_body_register_t) == 56, "register body size");

/* ── body: MSG_UNREGISTER ───────────────────────────────────────────── */

typedef struct {
	int32_t  pid;                   /*  4B */
	int32_t  tid;                   /*  4B */
} __attribute__((packed)) msg_body_unregister_t;

_Static_assert(sizeof(msg_body_unregister_t) == 8, "unregister body size");

/* ── body: MSG_HEARTBEAT ────────────────────────────────────────────── */

typedef struct {
	int32_t  pid;                   /*  4B */
	int32_t  tid;                   /*  4B */
} __attribute__((packed)) msg_body_heartbeat_t;

_Static_assert(sizeof(msg_body_heartbeat_t) == 8, "heartbeat body size");

/* ── body: MSG_SHUTDOWN ─────────────────────────────────────────────── */

typedef struct {
	int32_t  pid;                   /*  4B */
} __attribute__((packed)) msg_body_shutdown_t;

_Static_assert(sizeof(msg_body_shutdown_t) == 4, "shutdown body size");

/* ── body: MSG_RESPONSE ─────────────────────────────────────────────── */

typedef struct {
	uint8_t  status;                /*  1B */
	uint8_t  _reserved[3];          /*  3B */
} __attribute__((packed)) msg_body_response_t;

_Static_assert(sizeof(msg_body_response_t) == 4, "response body size");

/* ── status codes ───────────────────────────────────────────────────── */

#define MSG_STATUS_OK             0x00
#define MSG_STATUS_TABLE_FULL     0x01
#define MSG_STATUS_ALREADY_REG    0x02
#define MSG_STATUS_INVALID_ARG    0x03
#define MSG_STATUS_ERROR          0xFF

#ifdef __cplusplus
}
#endif

#endif
