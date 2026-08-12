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

#include "taskhealth_mutex.h"
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
}

void taskhealth_mutex_destroy(struct taskhealth_mutex *m)
{
	int i;

	if (!m)
		return;

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
	if (m)
		pthread_mutex_lock(&m->mutex);
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
