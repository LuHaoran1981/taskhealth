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
#include "probe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ── thread alive ──────────────────────────────────────────────────── */

int probe_thread_alive(pid_t pid, pid_t tid)
{
	long ret = syscall(SYS_tgkill, pid, tid, 0);
	if (ret == 0) return 1;
	if (errno == ESRCH) return 0;
	return -1;
}

/* ── /proc/<pid>/task/<tid>/status ──────────────────────────────────── */

char probe_thread_state(pid_t pid, pid_t tid)
{
	char path[64];
	char line[128];
	char state = '?';
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/status",
		 (int)pid, (int)tid);
	f = fopen(path, "r");
	if (!f) return '?';

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "State:", 6) == 0) {
			char *p = line + 6;
			while (*p == '\t' || *p == ' ') p++;
			state = *p;
			break;
		}
	}
	fclose(f);
	return state;
}

/* ── /proc/<pid>/task/<tid>/wchan ───────────────────────────────────── */

int probe_wchan(pid_t pid, pid_t tid, char *buf, size_t len)
{
	char path[64];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/wchan",
		 (int)pid, (int)tid);
	f = fopen(path, "r");
	if (!f) return 0;

	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return 0;
	}

	size_t slen = strlen(buf);
	if (slen > 0 && buf[slen - 1] == '\n')
		buf[slen - 1] = '\0';

	fclose(f);
	return (int)slen;
}

/* ── /proc/<pid>/task/<tid>/syscall ─────────────────────────────────── */

uintptr_t probe_futex_addr(pid_t pid, pid_t tid)
{
	char path[64];
	char line[256];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/syscall",
		 (int)pid, (int)tid);
	f = fopen(path, "r");
	if (!f) return 0;

	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	long      syscall_nr = 0;
	uintptr_t arg1       = 0;
	if (sscanf(line, "%ld %lx", &syscall_nr, &arg1) < 2)
		return 0;

#if defined(__x86_64__)
	if (syscall_nr != 202) return 0;
#elif defined(__aarch64__)
	if (syscall_nr != 98) return 0;
#elif defined(__arm__)
	if (syscall_nr != 240) return 0;
#elif defined(__i386__)
	if (syscall_nr != 240) return 0;
#else
	(void)syscall_nr;
#endif

	return arg1;
}

/* ── /proc/<pid>/maps ───────────────────────────────────────────────── */

char *probe_resolve_futex(pid_t pid, uintptr_t addr)
{
	char path[64];
	char line[512];
	FILE *f;

	if (addr == 0) return NULL;

	snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
	f = fopen(path, "r");
	if (!f) return NULL;

	while (fgets(line, sizeof(line), f)) {
		unsigned long start = 0, end = 0;

		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;

		if (addr < start || addr >= end)
			continue;

		char *p = strchr(line, '/');
		if (!p)
			p = strchr(line, '[');

		size_t plen;
		if (p) {
			plen = strlen(p);
			if (plen > 0 && p[plen - 1] == '\n')
				plen--;
		} else {
			p = "";
			plen = 0;
		}

		size_t len = plen + 32;
		char *result = malloc(len);
		if (result) {
			unsigned long offset = addr - start;
			snprintf(result, len, "%.*s+0x%lx",
				 (int)plen, p, offset);
		}
		fclose(f);
		return result;
	}

	fclose(f);
	return NULL;
}
