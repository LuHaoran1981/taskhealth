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

#define _GNU_SOURCE
#include "server.h"
#include "taskhealth/protocol.h"
#include "registry.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS     128
#define MAX_CLIENTS    256

static int g_listen_fd = -1;
static int g_epoll_fd = -1;
static _Atomic bool g_running;

/* fd → pid mapping for disconnect cleanup */
static pid_t g_fd2pid[MAX_CLIENTS];

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int send_response(int fd, uint8_t status)
{
	taskhealth_msg_hdr_t hdr = {
		.magic    = TASKHEALTH_MSG_MAGIC,
		.type     = MSG_RESPONSE,
		.body_len = sizeof(msg_body_response_t),
	};
	msg_body_response_t body = { .status = status };

	struct iovec iov[2] = {
		{ .iov_base = &hdr,  .iov_len = sizeof(hdr) },
		{ .iov_base = &body, .iov_len = sizeof(body) },
	};
	struct msghdr msg = {
		.msg_iov    = iov,
		.msg_iovlen = 2,
	};
	return (int)sendmsg(fd, &msg, MSG_NOSIGNAL);
}

static void handle_register(int fd, const msg_body_register_t *body)
{
	Entry *e = NULL;
	registry_add_result_t rc;

	if (body->gap_ms < 0 || body->lock_hold_ms < 0) {
		send_response(fd, MSG_STATUS_INVALID_ARG);
		return;
	}

	registry_lock();
	rc = registry_add(body, fd, &e);
	if (rc == REGISTRY_ADD_OK)
		e->last_heartbeat_ns = now_ns();
	registry_unlock();

	switch (rc) {
	case REGISTRY_ADD_OK:
		send_response(fd, MSG_STATUS_OK);
		break;
	case REGISTRY_ADD_DUPLICATE:
		send_response(fd, MSG_STATUS_ALREADY_REG);
		break;
	case REGISTRY_ADD_FULL:
		send_response(fd, MSG_STATUS_TABLE_FULL);
		break;
	default:
		send_response(fd, MSG_STATUS_ERROR);
		break;
	}
}

static void handle_heartbeat(const msg_body_heartbeat_t *body)
{
	registry_lock();
	registry_heartbeat(body->pid, body->tid, now_ns());
	registry_unlock();
}

static void handle_unregister(const msg_body_unregister_t *body)
{
	registry_lock();
	registry_remove(body->pid, body->tid);
	registry_unlock();
}

static void handle_mutex_register(int fd, const msg_body_mutex_register_t *body)
{
	/* anti-spoof: body->pid must match the connection's SO_PEERCRED pid */
	if (fd >= 0 && fd < MAX_CLIENTS && g_fd2pid[fd] != 0 &&
	    g_fd2pid[fd] != body->pid)
		return;

	registry_mutex_add(fd, body->futex_addr, body->name);
}

static void handle_mutex_unregister(int fd, const msg_body_mutex_unregister_t *body)
{
	if (fd >= 0 && fd < MAX_CLIENTS && g_fd2pid[fd] != 0 &&
	    g_fd2pid[fd] != body->pid)
		return;

	registry_mutex_remove(fd, body->futex_addr);
}

static void handle_lock_wait(int fd, const msg_body_lock_state_t *body)
{
	if (fd >= 0 && fd < MAX_CLIENTS && g_fd2pid[fd] != 0 &&
	    g_fd2pid[fd] != body->pid)
		return;

	registry_lock();
	registry_lock_wait(body->pid, body->tid, body->futex_addr);
	registry_unlock();
}

static void handle_lock_acquired(int fd, const msg_body_lock_state_t *body)
{
	if (fd >= 0 && fd < MAX_CLIENTS && g_fd2pid[fd] != 0 &&
	    g_fd2pid[fd] != body->pid)
		return;

	registry_lock();
	registry_lock_acquired(body->pid, body->tid);
	registry_unlock();
}

static void handle_shutdown(int fd, const msg_body_shutdown_t *body)
{
	registry_lock();
	registry_cleanup_pid(body->pid);
	registry_unlock();
	registry_mutex_cleanup_fd(fd);
	close(fd);
	if (fd >= 0 && fd < MAX_CLIENTS)
		g_fd2pid[fd] = 0;
}

static void handle_client_disconnect(int fd)
{
	registry_lock();
	registry_cleanup_fd(fd);
	registry_unlock();
	registry_mutex_cleanup_fd(fd);
	close(fd);
	if (fd >= 0 && fd < MAX_CLIENTS)
		g_fd2pid[fd] = 0;
}

static int recv_full_frame(int fd, taskhealth_msg_hdr_t *hdr, void **body_out)
{
	/* Read the full SEQPACKET message in one recv.
	 * Max message: 8B header + 4096B body = 4104B */
	char buf[4104];
	ssize_t r = recv(fd, buf, sizeof(buf), 0);
	if (r <= 0) return -1;
	if ((size_t)r < sizeof(*hdr)) return -1;

	memcpy(hdr, buf, sizeof(*hdr));

	if (hdr->magic != TASKHEALTH_MSG_MAGIC)
		return -1;
	if (hdr->body_len > 4096)
		return -1;
	if ((size_t)r < sizeof(*hdr) + hdr->body_len)
		return -1;
	if (hdr->body_len == 0) {
		*body_out = NULL;
		return 0;
	}

	char *body = malloc(hdr->body_len);
	if (!body) return -1;
	memcpy(body, buf + sizeof(*hdr), hdr->body_len);
	*body_out = body;
	return 0;
}

int server_init(const char *socket_path)
{
	struct sockaddr_un addr;

	g_listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (g_listen_fd < 0) return -1;

	unlink(socket_path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}

	if (listen(g_listen_fd, 32) < 0) {
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}

	g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epoll_fd < 0) {
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}

	struct epoll_event ev;
	ev.events  = EPOLLIN;
	ev.data.fd = g_listen_fd;
	epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &ev);

	memset(g_fd2pid, 0, sizeof(g_fd2pid));
	atomic_store(&g_running, true);
	return 0;
}

void server_run(void)
{
	struct epoll_event events[MAX_EVENTS];

	while (atomic_load(&g_running)) {
		int nfds = epoll_wait(g_epoll_fd, events, MAX_EVENTS, 1000);
		if (nfds < 0) {
			if (errno == EINTR) continue;
			break;
		}

		for (int i = 0; i < nfds; i++) {
			int fd = events[i].data.fd;

			if (fd == g_listen_fd) {
				/* accept */
				int client = accept4(g_listen_fd, NULL, NULL,
						    SOCK_CLOEXEC);
				if (client < 0) continue;

				/* SO_PEERCRED UID check */
				struct ucred cred;
				socklen_t clen = sizeof(cred);
				if (getsockopt(client, SOL_SOCKET, SO_PEERCRED,
					       &cred, &clen) == 0) {
					if (cred.uid != getuid()) {
						close(client);
						continue;
					}
					if (client < MAX_CLIENTS)
						g_fd2pid[client] = cred.pid;
				}

				struct epoll_event ev;
				ev.events  = EPOLLIN;
				ev.data.fd = client;
				if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD,
					      client, &ev) < 0) {
					close(client);
					continue;
				}
			} else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
				handle_client_disconnect(fd);
			} else if (events[i].events & EPOLLIN) {
				taskhealth_msg_hdr_t hdr;
				void *body = NULL;

				if (recv_full_frame(fd, &hdr, &body) < 0) {
					handle_client_disconnect(fd);
					continue;
				}

				switch (hdr.type) {
				case MSG_REGISTER:
					if (body && hdr.body_len >= sizeof(msg_body_register_t))
						handle_register(fd, (const msg_body_register_t *)body);
					break;
				case MSG_HEARTBEAT:
					if (body && hdr.body_len >= sizeof(msg_body_heartbeat_t))
						handle_heartbeat((const msg_body_heartbeat_t *)body);
					break;
				case MSG_UNREGISTER:
					if (body && hdr.body_len >= sizeof(msg_body_unregister_t))
						handle_unregister((const msg_body_unregister_t *)body);
					break;
				case MSG_SHUTDOWN:
					if (body && hdr.body_len >= sizeof(msg_body_shutdown_t))
						handle_shutdown(fd, (const msg_body_shutdown_t *)body);
					break;
				case MSG_MUTEX_REGISTER:
					if (body && hdr.body_len >= sizeof(msg_body_mutex_register_t))
						handle_mutex_register(fd, (const msg_body_mutex_register_t *)body);
					break;
				case MSG_MUTEX_UNREGISTER:
					if (body && hdr.body_len >= sizeof(msg_body_mutex_unregister_t))
						handle_mutex_unregister(fd, (const msg_body_mutex_unregister_t *)body);
					break;
				case MSG_LOCK_WAIT:
					if (body && hdr.body_len >= sizeof(msg_body_lock_state_t))
						handle_lock_wait(fd, (const msg_body_lock_state_t *)body);
					break;
				case MSG_LOCK_ACQUIRED:
					if (body && hdr.body_len >= sizeof(msg_body_lock_state_t))
						handle_lock_acquired(fd, (const msg_body_lock_state_t *)body);
					break;
				default:
					break;
				}

				free(body);
			}
		}
	}
}

void server_request_stop(void)
{
	atomic_store(&g_running, false);
}

void server_stop(void)
{
	server_request_stop();
	if (g_epoll_fd >= 0) {
		close(g_epoll_fd);
		g_epoll_fd = -1;
	}
	if (g_listen_fd >= 0) {
		close(g_listen_fd);
		g_listen_fd = -1;
	}
}
