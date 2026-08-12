# TaskHealth 需求文档

## 1. 背景与目标

QNX 提供 HAM (High Availability Manager) 管理进程健康，Linux 提供 systemd 管理进程健
康。两者均不覆盖线程级别的健康监控。

TaskHealth 定位为 Linux 用户态线程健康监控库，填补 QNX HAM / systemd 在线程层面的空白。

## 2. 监控的故障类型（MVP）

| 编号 | 故障类型 | 描述 |
|------|---------|------|
| F1 | 线程意外退出 | 线程因 `pthread_exit`、`pthread_cancel`、入口函数 return 等原因退出，进程仍存活 |
| F2 | 死锁 | 线程长时间阻塞在 futex（`pthread_mutex_lock` 等用户态锁）上 |

不在 MVP 范围：活锁（R 状态不干活）、CPU 饥饿、D 状态长阻塞（内核 hung task detector 已覆盖）。

### 2.1 监控范围声明

本工具监控基于 futex 的用户态同步原语。由于 `pthread_mutex_t`、`sem_t`（glibc）等底层均使用 futex，内核视角无法区分同步原语类型。因此：

- **主要监控目标**：`pthread_mutex_lock` 阻塞
- **可能附带检测到但不区分**：`sem_wait`、`pthread_cond_wait`、`pthread_rwlock_*` 等在 futex 上阻塞的场景
- **不专门监控**：信号量（semaphore）不管控，不作为独立监控类型

## 3. 功能需求

### FR1: 线程注册

- 只有显式注册的线程才被监控，不监控所有线程
- 注册时提供：
  - `name`：线程名称（用于告警信息）
  - `gap_ms`：监控间隔（心跳超时阈值）。0 表示不启用心跳检测
  - `lock_hold_ms`：等锁超时阈值。0 表示不检测等锁
- 线程退出前可调用注销接口
- 注册失败时有明确错误码（表满、已注册、未初始化等）

### FR2: 线程退出检测（gap 任意值）

- 所有注册线程均受退出检测
- 检测方式：周期性调用 `pthread_kill(tid, 0)`，返回 `ESRCH` 即判定线程已退出
- 检测到退出后触发 `on_unexpected_exit` 回调
- 同一线程的同一次退出只告警一次

### FR3: 心跳检测（gap > 0）

- 线程在循环中调用 `taskhealth_heartbeat()` 更新时间戳
- 提供心跳接口，线程自行决定调用时机
- Watchdog 周期性扫描：心跳超过 `gap_ms` 未更新即判定超时
- 心跳时间戳使用 `CLOCK_MONOTONIC`，不受系统时间跳变影响

### FR4: 死锁检测（gap > 0）

- 心跳超时后，若线程仍存在（`pthread_kill(tid, 0) == 0`），进一步判断是否死锁
- 读取 `/proc/self/task/<tid>/status` 的 `State` 字段，确认线程处于 S 或 D 状态
- 读取 `/proc/self/task/<tid>/wchan`，确认阻塞在 futex 相关函数
- 两个条件均满足 → 触发 `on_deadlock` 回调
- 告警时同时采集以下现场信息：
  - futex 地址（从 `/proc/self/task/<tid>/syscall` 第1参数获取）
  - futex 地址所属模块及偏移（从 `/proc/self/maps` 解析），如 `libmylib.so+0x80c0`
  - 锁名（若能匹配到注册的 `taskhealth_mutex_t`）
- 同一线程的同一次死锁只告警一次，心跳恢复后重置

### FR5: 等锁超时检测（lock_hold_ms > 0）

- 独立于心跳机制，每次 watchdog 扫描时检查
- 读取 `/proc/self/task/<tid>/wchan` 判断是否在等 futex
- 若在等 futex，累计等待时长；若不再等待，清零
- 累计等待时长超过 `lock_hold_ms` → 触发 `on_lock_wait_timeout` 回调
- 告警信息中包含：
  - 线程名和 TID
  - futex 地址（从 `/proc/self/task/<tid>/syscall` 第1参数获取）
  - futex 地址所属模块及偏移（从 `/proc/self/maps` 解析）
  - 等待时长
  - 锁名（若能匹配到注册的 `taskhealth_mutex_t`）

### FR6: 告警回调

- 提供默认实现（输出 stderr），允许用户覆盖
- 三种回调类型：
  - `on_unexpected_exit(name, tid)`
  - `on_deadlock(name, tid, wchan, futex_addr, lock_name, futex_module)`
  - `on_lock_wait_timeout(name, tid, futex_addr, wait_ms, lock_name, futex_module)`
- 同一事件不重复告警，事件恢复后重置标记

### FR7: 生命周期管理

- `taskhealth_init(cfg)`：初始化 watchdog，cfg 为 NULL 时使用默认值
- `taskhealth_shutdown()`：停止 watchdog，释放所有资源
- 初始化/关闭幂等

## 4. 非功能需求

| 需求 | 描述 |
|------|------|
| 性能 | 心跳更新为 O(1)、无锁操作；注册/注销持锁时间短 |
| 可移植性 | 纯 C99 + POSIX，无外部依赖，Linux 3.x+ 内核 |
| 线程安全 | 并发注册/注销/心跳互不阻塞 |
| 资源 | 固定内存（线程表），不动态分配，零运行期 malloc（初始化时分配） |

## 5. 参数组合矩阵

| gap_ms | lock_hold_ms | 检测能力 |
|--------|:-----------:|---------|
| 0 | 0 | 仅 `pthread_kill` 周期性检查线程退出 |
| >0 | 0 | 心跳 + 退出检测 + 心跳超时时死锁确认 |
| >0 | >0 | 以上全部 + 等锁超时检测 |
| 0 | >0 | 退出检测 + 等锁超时检测 |

## 6. 不做什么

- 不做自动重启线程（线程共享地址空间，无法安全恢复）
- 不做 LD_PRELOAD 拦截（避免 License 边界问题）
- 不做外部 daemon（先走进程内库）
- 不做 libunwind 用户态栈回溯（使用 `/proc/self/maps` 解析 futex 地址所属模块替代）
- 不做信号量（semaphore）独立监控（`sem_wait` 底层也是 futex，会顺带检测到但不管控）

---

Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
上海先道智觉科技有限责任公司 | Author: Lu Haoran 芦浩然
SPDX-License-Identifier: GPL-2.0-or-later
