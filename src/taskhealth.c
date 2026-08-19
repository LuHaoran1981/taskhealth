/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <luhaoran1981@icloud.com>
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

#define _GNU_SOURCE
#include "taskhealth.h"
#include "taskhealth/protocol.h"
#include "taskhealth_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCK_PATH  "/tmp/taskhealth.sock"

/* Bound the synchronous register response wait so a hung daemon cannot
 * block the caller indefinitely. */
#define REGISTER_RESPONSE_TIMEOUT_SEC 2

/* ── global client state ────────────────────────────────────────────── */

static struct {
	int         sock_fd;
	pid_t       pid;
	bool        initialized;
	pthread_key_t tls_key;
} g_client;

/* ── helpers ────────────────────────────────────────────────────────── */

static pid_t get_tid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

static taskhealth_msg_hdr_t build_hdr(uint8_t type, uint32_t body_len)
{
	taskhealth_msg_hdr_t h = {
		.magic    = TASKHEALTH_MSG_MAGIC,
		.type     = type,
		.body_len = body_len,
	};
	return h;
}

static int send_msg(int fd, const taskhealth_msg_hdr_t *hdr, const void *body)
{
	struct iovec iov[2] = {
		{ .iov_base = (void *)hdr,  .iov_len = sizeof(*hdr) },
		{ .iov_base = (void *)body, .iov_len = hdr->body_len },
	};
	struct msghdr msg = {
		.msg_iov    = iov,
		.msg_iovlen = 2,
	};
	return (int)sendmsg(fd, &msg, MSG_NOSIGNAL);
}

static int recv_exact(int fd, void *buf, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t r = recv(fd, (char *)buf + off, n - off, 0);
		if (r <= 0) return -1;
		off += (size_t)r;
	}
	return 0;
}

static int do_connect(const char *path)
{
	struct sockaddr_un addr;

	int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0) return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	memcpy(addr.sun_path, path, strlen(path) + 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	struct timeval tv = { .tv_sec = REGISTER_RESPONSE_TIMEOUT_SEC, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	return fd;
}

/* ── public API ─────────────────────────────────────────────────────── */

const char *taskhealth_version(void)
{
	return TASKHEALTH_VERSION;
}

int taskhealth_init(const struct taskhealth_config *cfg)
{
	const char *path;

	if (g_client.initialized)
		return TASKHEALTH_ERR_ALREADY_INIT;

	path = (cfg && cfg->socket_path[0]) ? cfg->socket_path : DEFAULT_SOCK_PATH;

	g_client.sock_fd = do_connect(path);
	if (g_client.sock_fd < 0)
		return TASKHEALTH_ERR_NOT_INIT;

	g_client.pid = getpid();
	pthread_key_create(&g_client.tls_key, NULL);
	g_client.initialized = true;
	return TASKHEALTH_OK;
}

void taskhealth_shutdown(void)
{
	if (!g_client.initialized)
		return;

	msg_body_shutdown_t body = { .pid = g_client.pid };
	taskhealth_msg_hdr_t hdr = build_hdr(MSG_SHUTDOWN, sizeof(body));
	send_msg(g_client.sock_fd, &hdr, &body);

	close(g_client.sock_fd);
	g_client.sock_fd = -1;
	pthread_key_delete(g_client.tls_key);
	g_client.initialized = false;
}

int taskhealth_register(const char *name, int64_t gap_ms, int64_t lock_hold_ms)
{
	char auto_name[TASKHEALTH_NAME_LEN];

	if (!g_client.initialized)
		return TASKHEALTH_ERR_NOT_INIT;
	if (gap_ms < 0 || lock_hold_ms < 0)
		return TASKHEALTH_ERR_INVALID_ARG;
	if (pthread_getspecific(g_client.tls_key) != NULL)
		return TASKHEALTH_ERR_ALREADY_REG;

	/* auto-name: /proc/self/comm + TID */
	if (!name || name[0] == '\0') {
		auto_name[0] = '\0';
		FILE *f = fopen("/proc/self/comm", "r");
		if (f) {
			if (fgets(auto_name, sizeof(auto_name), f)) {
				size_t len = strlen(auto_name);
				if (len > 0 && auto_name[len - 1] == '\n')
					auto_name[len - 1] = '\0';
			}
			fclose(f);
		}
		if (auto_name[0] == '\0')
			snprintf(auto_name, sizeof(auto_name), "tid.%d",
				 (int)get_tid());
		else
			snprintf(auto_name + strlen(auto_name),
				 sizeof(auto_name) - strlen(auto_name),
				 ".%d", (int)get_tid());
		name = auto_name;
	}

	msg_body_register_t body = {
		.pid          = g_client.pid,
		.tid          = (int32_t)get_tid(),
		.gap_ms       = gap_ms,
		.lock_hold_ms = lock_hold_ms,
	};
	strncpy(body.name, name, sizeof(body.name) - 1);
	body.name[sizeof(body.name) - 1] = '\0';

	taskhealth_msg_hdr_t hdr = build_hdr(MSG_REGISTER, sizeof(body));
	if (send_msg(g_client.sock_fd, &hdr, &body) < 0) {
		close(g_client.sock_fd);
		g_client.sock_fd = -1;
		g_client.initialized = false;
		return TASKHEALTH_ERR_NOT_INIT;
	}

	char resp_buf[sizeof(taskhealth_msg_hdr_t) + sizeof(msg_body_response_t)];
	if (recv_exact(g_client.sock_fd, resp_buf, sizeof(resp_buf)) < 0) {
		close(g_client.sock_fd);
		g_client.sock_fd = -1;
		g_client.initialized = false;
		return TASKHEALTH_ERR_NOT_INIT;
	}

	const msg_body_response_t *resp =
		(const msg_body_response_t *)(resp_buf + sizeof(taskhealth_msg_hdr_t));

	if (resp->status != MSG_STATUS_OK) {
		switch (resp->status) {
		case MSG_STATUS_TABLE_FULL:   return TASKHEALTH_ERR_TABLE_FULL;
		case MSG_STATUS_ALREADY_REG:  return TASKHEALTH_ERR_ALREADY_REG;
		case MSG_STATUS_INVALID_ARG:  return TASKHEALTH_ERR_INVALID_ARG;
		default:                      return TASKHEALTH_ERR_NOT_INIT;
		}
	}

	pthread_setspecific(g_client.tls_key, (void *)1);
	return TASKHEALTH_OK;
}

void taskhealth_unregister(void)
{
	if (pthread_getspecific(g_client.tls_key) == NULL)
		return;

	msg_body_unregister_t body = {
		.pid = g_client.pid,
		.tid = (int32_t)get_tid(),
	};
	taskhealth_msg_hdr_t hdr = build_hdr(MSG_UNREGISTER, sizeof(body));
	send_msg(g_client.sock_fd, &hdr, &body);

	pthread_setspecific(g_client.tls_key, NULL);
}

void taskhealth_heartbeat(void)
{
	if (pthread_getspecific(g_client.tls_key) == NULL)
		return;

	msg_body_heartbeat_t body = {
		.pid = g_client.pid,
		.tid = (int32_t)get_tid(),
	};
	taskhealth_msg_hdr_t hdr = build_hdr(MSG_HEARTBEAT, sizeof(body));

	ssize_t ret = send_msg(g_client.sock_fd, &hdr, &body);
	if (ret < 0 && errno == EPIPE) {
		close(g_client.sock_fd);
		g_client.sock_fd = -1;
		g_client.initialized = false;
	}
}

/* ── internal mutex-name hooks (called by taskhealth_mutex.c) ───────── */

void taskhealth_mutex_notify_register(uintptr_t futex_addr, const char *name)
{
	msg_body_mutex_register_t body;
	taskhealth_msg_hdr_t hdr;

	if (!g_client.initialized)
		return;

	memset(&body, 0, sizeof(body));
	body.pid = g_client.pid;
	body.futex_addr = (uint64_t)futex_addr;
	if (name)
		strncpy(body.name, name, sizeof(body.name) - 1);

	hdr = build_hdr(MSG_MUTEX_REGISTER, sizeof(body));
	send_msg(g_client.sock_fd, &hdr, &body);
}

void taskhealth_mutex_notify_unregister(uintptr_t futex_addr)
{
	msg_body_mutex_unregister_t body;
	taskhealth_msg_hdr_t hdr;

	if (!g_client.initialized)
		return;

	body.pid = g_client.pid;
	body.futex_addr = (uint64_t)futex_addr;

	hdr = build_hdr(MSG_MUTEX_UNREGISTER, sizeof(body));
	send_msg(g_client.sock_fd, &hdr, &body);
}

static void mutex_notify_lock_state(uint8_t type, uintptr_t futex_addr)
{
	msg_body_lock_state_t body;
	taskhealth_msg_hdr_t hdr;

	if (!g_client.initialized)
		return;

	body.pid = g_client.pid;
	body.tid = (int32_t)get_tid();
	body.futex_addr = (uint64_t)futex_addr;

	hdr = build_hdr(type, sizeof(body));
	send_msg(g_client.sock_fd, &hdr, &body);
}

void taskhealth_mutex_notify_lock_wait(uintptr_t futex_addr)
{
	mutex_notify_lock_state(MSG_LOCK_WAIT, futex_addr);
}

void taskhealth_mutex_notify_lock_acquired(uintptr_t futex_addr)
{
	mutex_notify_lock_state(MSG_LOCK_ACQUIRED, futex_addr);
}
