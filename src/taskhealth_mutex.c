/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
 *                   上海先道智觉科技有限责任公司
 * Author: Lu Haoran <37183985@qq.com>
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

#include "taskhealth_mutex.h"
#include "taskhealth_internal.h"
#include <stdlib.h>
#include <string.h>

static struct {
	struct taskhealth_mutex	*mutexes[TASKHEALTH_MAX_MUTEXES];
	int			count;
	pthread_mutex_t		lock;
} g_mutex_registry = { .count = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

void taskhealth_mutex_init(struct taskhealth_mutex *m, const char *name)
{
	if (!m)
		return;
	pthread_mutex_init(&m->mutex, NULL);
	m->name       = name;
	m->addr       = (uintptr_t)&m->mutex;
	m->registered = false;

	pthread_mutex_lock(&g_mutex_registry.lock);
	if (g_mutex_registry.count < TASKHEALTH_MAX_MUTEXES) {
		g_mutex_registry.mutexes[g_mutex_registry.count++] = m;
		m->registered = true;
	}
	pthread_mutex_unlock(&g_mutex_registry.lock);

	taskhealth_mutex_notify_register(m->addr, name);
}

void taskhealth_mutex_destroy(struct taskhealth_mutex *m)
{
	int i;

	if (!m)
		return;

	taskhealth_mutex_notify_unregister(m->addr);

	pthread_mutex_lock(&g_mutex_registry.lock);
	for (i = 0; i < g_mutex_registry.count; i++) {
		if (g_mutex_registry.mutexes[i] == m) {
			g_mutex_registry.mutexes[i] =
				g_mutex_registry.mutexes[--g_mutex_registry.count];
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex_registry.lock);

	m->registered = false;
	pthread_mutex_destroy(&m->mutex);
}

void taskhealth_mutex_lock(struct taskhealth_mutex *m)
{
	if (!m)
		return;

	/* fast path: uncontended lock takes no IPC */
	if (pthread_mutex_trylock(&m->mutex) == 0)
		return;

	/* contended → tell the daemon which futex we are about to block on */
	taskhealth_mutex_notify_lock_wait(m->addr);
	pthread_mutex_lock(&m->mutex);
	taskhealth_mutex_notify_lock_acquired(m->addr);
}

void taskhealth_mutex_unlock(struct taskhealth_mutex *m)
{
	if (m)
		pthread_mutex_unlock(&m->mutex);
}

const char *taskhealth_mutex_resolve(uintptr_t futex_addr)
{
	int i;
	const char *ret = NULL;

	pthread_mutex_lock(&g_mutex_registry.lock);
	for (i = 0; i < g_mutex_registry.count; i++) {
		struct taskhealth_mutex *m = g_mutex_registry.mutexes[i];
		if (m->addr == futex_addr) {
			ret = m->name;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex_registry.lock);
	return ret;
}
