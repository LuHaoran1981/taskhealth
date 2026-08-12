# TaskHealth 项目立项书

## 1. 项目概述

### 1.1 项目名称

TaskHealth —— Linux 用户态线程健康监控库

### 1.2 项目背景

在嵌入式及高可靠性系统中，进程健康监控已有成熟方案：

| 系统 | 工具 | 监控粒度 |
|------|------|---------|
| QNX | HAM (High Availability Manager) | 进程 |
| Linux | systemd | 进程 |

两者均不覆盖**线程级别**的健康监控。当进程内某个线程因死锁、意外退出等原因失效时，进程整体仍存活，systemd 无法感知，形成监控盲区。

本项目受 QNX HAM 设计理念启发，在 Linux 平台上填补线程健康监控的空白。

### 1.3 项目目标

开发一个轻量级、零依赖的 C 语言库，提供以下核心能力：

1. **线程退出检测**：感知被监控线程的意外退出
2. **死锁检测**：感知线程在 futex 上的长时间阻塞
3. **等锁超时检测**：感知线程等待锁超过业务可接受阈值
4. **现场信息采集**：告警时自动记录 futex 地址、内核栈回溯、锁名等信息

### 1.4 应用场景

- 自动驾驶/车载系统（对标 QNX HAM 的部分能力）
- 机器人控制系统
- 金融交易系统
- 任何对线程健康有监控需求的 Linux 服务端程序

## 2. 项目范围

### 2.1 范围内（MVP）

| 编号 | 功能 | 说明 |
|------|------|------|
| F1 | 线程注册/注销 | 按线程粒度注册，不监控所有线程 |
| F2 | 退出检测 | `pthread_kill(tid, 0)` 周期性检查 |
| F3 | 心跳机制 | gap_ms > 0 时启用，原子变量无锁更新 |
| F4 | 死锁检测 | 心跳超时 + /proc 状态 + wchan 确认 |
| F5 | 等锁超时检测 | /proc wchan + syscall 探针，独立于心跳 |
| F6 | 现场信息采集 | futex 地址、内核栈、锁名 |
| F7 | 告警回调 | 三种回调类型，默认 stderr，可覆盖 |
| F8 | 生命周期管理 | init/shutdown 幂等 |

### 2.2 范围外（MVP 不做）

- 线程自动重启（线程共享地址空间，无法安全恢复）
- 活锁/CPU 饥饿检测（线程在 R 状态，内核视角正常）
- 信号量独立监控（`sem_wait` 底层也是 futex，会附带检测但不区分类型）
- 外部 daemon 监控模式（先做进程内库）
- libunwind 用户态栈回溯（使用 /proc 内核栈替代）
- LD_PRELOAD 拦截方案（避免 License 边界风险）

### 2.3 监控范围声明

本工具基于 Linux futex 机制检测。由于 `pthread_mutex_t`、`sem_t`（glibc）、`pthread_cond_t`、`pthread_rwlock_t` 等用户态同步原语底层均使用 futex 系统调用，内核视角无法区分具体类型。因此：

- **主要监控目标**：`pthread_mutex_lock` 阻塞
- **可能附带检测（不区分类型）**：`sem_wait`、`pthread_cond_wait`、`pthread_rwlock_*` 等
- **不专门监控**：信号量作为独立监控目标不在范围内

## 3. 技术方案

### 3.1 总体架构

进程内 watchdog 线程 + 注册表 + /proc 探针。不依赖外部 daemon、不依赖新内核特性（Linux 3.x+ 即可）。

```
工作线程 ──heartbeat()──→ 注册表(原子写) ←── Watchdog 线程 ──→ /proc 探针
                              │                        │
                              └────────────────────────┘
                                         │
                                   告警回调 → stderr / 用户自定义
```

### 3.2 关键技术选型

| 决策 | 方案 | 理由 |
|------|------|------|
| 退出检测 | `pthread_kill(tid, 0)` | 单次 syscall ~100ns，无需开文件 |
| 心跳时间 | `clock_gettime(CLOCK_MONOTONIC)` | 不受系统时间跳变影响 |
| 心跳写入 | `_Atomic int64_t` 无锁 | 高频调用路径零阻塞 |
| 死锁确认 | `/proc/self/task/<tid>/status` + `wchan` | 内核视角，零业务侵入 |
| 等锁检测 | `/proc/self/task/<tid>/syscall` | 获取 futex 地址，定位问题锁 |
| 栈回溯 | `/proc/self/task/<tid>/stack` | 内核栈，无需 libunwind |
| 线程查找 | `pthread_key_t` TLS | O(1)，无锁 |
| 注册保护 | `pthread_mutex_t` | 低频操作，持锁时间短 |

### 3.3 API 设计

```c
int  taskhealth_init(const TaskHealthConfig *cfg);
void taskhealth_shutdown(void);
int  taskhealth_register(const char *name, int64_t gap_ms, int64_t lock_hold_ms);
void taskhealth_unregister(void);
void taskhealth_heartbeat(void);
```

### 3.4 技术指标

| 指标 | 目标值 |
|------|--------|
| 代码量（MVP） | < 800 行 C |
| 外部依赖 | 零（仅 C 标准库 + POSIX） |
| 运行时内存 | 约 16KB（128 线程默认配置） |
| 心跳开销 | 一次 `clock_gettime` + 一次原子写 |
| Watchdog CPU | 视注册线程数，128 线程约 < 1% CPU |
| 内核版本要求 | Linux 3.x+ |

## 4. 项目计划

### 4.1 里程碑

| 阶段 | 内容 | 预计产出 |
|------|------|---------|
| M1: 设计 | 需求、方案、详细设计评审 | 三份设计文档 |
| M2: 核心实现 | taskhealth.h/c + Makefile | 可编译的静态库 |
| M3: 验证 | demo 覆盖退出、死锁、等锁超时场景 | 全部场景通过 |
| M4: 文档 | API 文档、使用指南 | README + 示例代码 |

### 4.2 交付物

```
TaskHealth/
├── taskhealth.h        # 公开 API 头文件
├── taskhealth.c        # 核心实现
├── taskhealth_mutex.h  # 配套互斥锁（可选）
├── taskhealth_mutex.c
├── demo.c              # 演示程序
├── Makefile            # 构建系统
├── README.md           # 使用文档
├── REQUIREMENTS.md     # 需求文档
├── DESIGN.md           # 总体设计
├── DETAILED_DESIGN.md  # 详细设计
└── PROJECT_CHARTER.md  # 本文件
```

## 5. 风险评估

| 风险 | 影响 | 应对措施 |
|------|------|---------|
| /proc 文件系统在容器中受限 | 死锁/等锁检测失效 | 优雅降级，退出检测仍可用 |
| TID 复用导致误判 | 退出检测误报 | 概率极低（pid_max=4194304），可后续加 starttime 校验 |
| 高频扫描影响业务性能 | 业务延迟增加 | 扫描间隔可配，快照+释放锁后扫描 |
| 内核版本差异导致 wchan 格式变化 | 死锁误判 | 多内核版本测试，模糊匹配 futex 关键字 |

## 6. 知识产权声明

本项目为独立原创开发，参考以下公开信息：

- Linux `/proc` 文件系统文档（kernel.org）
- POSIX 线程标准（IEEE 1003.1）
- QNX HAM 设计理念（功能概念参考，非代码参考）

本项目的设计思路（HAM 式注册-监控模型）为通用软件架构模式，不受知识产权保护。所有代码将独立编写，不使用任何第三方受版权保护的源代码。

## 7. 审批

| 角色 | 签字 |
|------|------|
| 项目负责人 | |
| 技术负责人 | |
| 日期 | 2026-08-07 |

---

Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
上海先道智觉科技有限责任公司 | Author: Lu Haoran 芦浩然
SPDX-License-Identifier: GPL-2.0-or-later
