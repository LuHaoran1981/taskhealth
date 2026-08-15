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
#include "alert.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static char alert_script[256];
static FILE *alert_log_fp;

int alert_init(const char *script_path, const char *log_file)
{
	openlog("taskhealthd", LOG_PID, LOG_DAEMON);
	if (script_path && script_path[0]) {
		strncpy(alert_script, script_path, sizeof(alert_script) - 1);
		alert_script[sizeof(alert_script) - 1] = '\0';
	} else {
		alert_script[0] = '\0';
	}

	alert_log_fp = NULL;
	if (log_file && log_file[0]) {
		alert_log_fp = fopen(log_file, "a");
		if (!alert_log_fp)
			fprintf(stderr, "[taskhealthd] cannot open log file %s: %s\n",
				log_file, strerror(errno));
	}
	return 0;
}

static enum alert_severity severity_of(enum alert_type type)
{
	switch (type) {
	case ALERT_EXIT:
	case ALERT_DEADLOCK:
	case ALERT_LOCK_WAIT:
		return SEV_CRITICAL;
	default:
		return SEV_NORMAL;
	}
}

void alert_emit(enum alert_type type, const Entry *e,
		const char *wchan, uintptr_t futex_addr,
		int64_t wait_ms, const char *futex_module,
		const char *lock_name)
{
	char buf[512];
	const char *type_str;

	switch (type) {
	case ALERT_EXIT:
		type_str = "UNEXPECTED_EXIT";
		snprintf(buf, sizeof(buf),
			 "thread '%s' pid=%d tid=%d unexpectedly exited",
			 e->name, (int)e->pid, (int)e->tid);
		break;
	case ALERT_DEADLOCK:
		type_str = "DEADLOCK";
		snprintf(buf, sizeof(buf),
			 "thread '%s' pid=%d tid=%d blocked on futex "
			 "wchan=%s futex=0x%lx module=%s lock=%s",
			 e->name, (int)e->pid, (int)e->tid,
			 wchan ? wchan : "?",
			 (unsigned long)futex_addr,
			 futex_module ? futex_module : "?",
			 lock_name ? lock_name : "(unknown)");
		break;
	case ALERT_LOCK_WAIT:
		type_str = "LOCK_WAIT";
		snprintf(buf, sizeof(buf),
			 "thread '%s' pid=%d tid=%d lock-wait timeout "
			 "waited=%ldms futex=0x%lx module=%s lock=%s",
			 e->name, (int)e->pid, (int)e->tid,
			 (long)wait_ms, (unsigned long)futex_addr,
			 futex_module ? futex_module : "?",
			 lock_name ? lock_name : "(unknown)");
		break;
	default:
		return;
	}

	enum alert_severity sev = severity_of(type);
	int priority = (sev == SEV_CRITICAL) ? LOG_ERR : LOG_WARNING;

	/* 1. syslog (always) */
	syslog(priority, "[%s] %s", type_str, buf);

	/* 2. log file (always, if configured) */
	if (alert_log_fp) {
		char ts[32];
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
		fprintf(alert_log_fp, "%s [%s] [%s] %s\n", ts,
			(sev == SEV_CRITICAL) ? "CRIT" : "NORM", type_str, buf);
		fflush(alert_log_fp);
	}

	/* 3. stderr (critical only) */
	if (sev == SEV_CRITICAL)
		fprintf(stderr, "[taskhealthd] %s %s\n", type_str, buf);

	/* 4. external script (critical only) */
	if (sev == SEV_CRITICAL && alert_script[0]) {
		pid_t child = fork();
		if (child == 0) {
			char pid_str[16], tid_str[16], wait_str[32], addr_str[32];

			snprintf(pid_str,  sizeof(pid_str),  "%d", (int)e->pid);
			snprintf(tid_str,  sizeof(tid_str),  "%d", (int)e->tid);
			snprintf(wait_str, sizeof(wait_str), "%ld", (long)wait_ms);
			snprintf(addr_str, sizeof(addr_str), "0x%lx",
				 (unsigned long)futex_addr);

			setenv("TASKHEALTH_TYPE",   type_str, 1);
			setenv("TASKHEALTH_NAME",   e->name,  1);
			setenv("TASKHEALTH_PID",    pid_str,  1);
			setenv("TASKHEALTH_TID",    tid_str,  1);
			setenv("TASKHEALTH_WAIT_MS", wait_str, 1);
			setenv("TASKHEALTH_MODULE", futex_module ? futex_module : "", 1);
			setenv("TASKHEALTH_LOCK",   lock_name ? lock_name : "", 1);
			setenv("TASKHEALTH_FUTEX",  addr_str, 1);

			execl(alert_script, alert_script, NULL);
			_exit(1);
		}
		/* parent does not waitpid — non-blocking */
	}
}
