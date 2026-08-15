# TaskHealth

> QNX HAM for Linux user-space · Linux 用户态线程健康监控

TaskHealth is a **thread-level health monitor** for Linux user-space. It detects
unexpected thread exits, futex deadlocks, and lock-wait timeouts — a layer that
Linux lacks today (systemd only supervises *processes*, not *threads*).

TaskHealth 是 Linux 用户态的**线程级健康监控**：检测线程意外退出、futex 死锁、
等锁超时。Linux 目前只有 systemd 做进程级守护，线程这一层是空白。

## What is TaskHealth

TaskHealth is inspired by **QNX High Availability Manager (HAM)** — QNX's
built-in service supervisor that keeps critical processes alive. TaskHealth
brings the same "watch and act on failures" idea to Linux, but operates at the
**thread** granularity and **across processes**.

TaskHealth 的灵感来自 **QNX HAM**（QNX 的高可用管理器，负责监控并拉起关键进程）。
TaskHealth 把"监控故障并响应"这一思路带到 Linux，但粒度细化到**线程**，且支持
**跨进程**监控。

| | QNX HAM | TaskHealth |
|---|---|---|
| OS | QNX Neutrino | Linux |
| Granularity 粒度 | Process / service | **Thread** |
| Scope 范围 | Node / cluster | Single host, cross-process |
| Detects 检测 | Process crash / hang | Thread exit, futex deadlock, lock timeout |
| Action 响应 | Restart / failover | Alert (stderr / syslog / log file / script) |
| Recovery 恢复 | Automatic restart | Alert only (restart the process) |

On Linux, `systemd` already supervises *processes* (start / stop / restart), but
has no notion of individual *threads* inside a process. TaskHealth fills that
gap: it watches threads across processes and alerts when a single thread dies
or deadlocks — the granularity systemd does not cover.

Linux 上 `systemd` 已经做了进程级守护（启动 / 停止 / 拉起），但对进程内部的
单个线程没有任何概念。TaskHealth 补上这一层：跨进程监控线程，在单个线程退出
或死锁时告警——这是 systemd 覆盖不到的粒度。

**Architecture**（客户端-守护进程）: the daemon `taskhealthd` accepts
registrations from multiple processes over a Unix domain socket and runs the
watchdog detection engine (`/proc` probes); the client library `libtaskhealth`
links into business processes and sends register / heartbeat / unregister
messages over IPC.

守护进程 `taskhealthd` 通过 Unix 域套接字接受多进程注册，统一执行心跳检测、
`/proc` 探针和告警；客户端库 `libtaskhealth` 链接进业务进程，通过 IPC 发送
注册 / 心跳 / 注销消息。

## Dependencies 依赖

- Linux 3.x+, `/proc` filesystem required
- GCC (C11), GNU Make
- No external library dependencies

- Linux 3.x+，需要 `/proc` 文件系统
- GCC（C11），GNU Make
- 无外部库依赖

## Build 构建

```sh
make          # libtaskhealth.a/.so + taskhealthd
make demo     # demo 业务示例程序
make install  # 安装到 /usr/local
```

## Quick Start 快速上手

Terminal 1 — start the daemon:

终端 1 — 启动守护进程：

```sh
./taskhealthd -s /tmp/taskhealth.sock -i 500 -l /tmp/taskhealth.log
```

(`-l` 可选：告警同时写入日志文件；不加则只写 syslog 和 stderr。)

Terminal 2 — run the demo business program:

终端 2 — 运行 demo 业务程序：

```sh
LD_LIBRARY_PATH=. ./demo/demo
```

See [demo/demo.c](demo/demo.c) for usage; API docs are in
[src/taskhealth.h](src/taskhealth.h).

业务代码示例见 [demo/demo.c](demo/demo.c)，API 文档见 [src/taskhealth.h](src/taskhealth.h)。

## Packaging 打包

```sh
make deb  # Debian / Ubuntu
make rpm  # Fedora / RHEL
```

A Yocto recipe is also available at `contrib/yocto/`.

Yocto recipe 见 `contrib/yocto/`。

## Related Work — ARINC 653 Health Monitoring 对标（可选阅读）

> Aviation / safety-critical readers only — others may skip this section.
> 仅面向航空 / 安全关键领域读者，其他读者可跳过本节。

ARINC 653 (the avionics RTOS standard) defines a Health Monitor (HM) that
handles faults at module / partition / process levels. TaskHealth follows the
same spirit at the Linux thread level: a central monitor observes registered
threads and reports failures, rather than leaving each thread to fend for
itself.

ARINC 653（航空电子 RTOS 标准）定义了健康监控（HM），在模块 / 分区 / 进程三级
处理故障。TaskHealth 在 Linux 线程级延续了同样的思路：由中心监控器观察已注册
线程并上报故障，而非让每个线程自生自灭。

## License 许可

The client library (`src/`, i.e. `libtaskhealth.so` / `.a`) is licensed under
the **MIT license** — link it into your own software freely, no copyleft.
See [LICENSE.MIT](LICENSE.MIT).

The daemon (`daemon/`, `taskhealthd`) is **GPL-2.0-or-later**.
See [LICENSE](LICENSE).

客户端库（`src/`，即 `libtaskhealth.so` / `.a`）采用 **MIT 协议**，可自由链接进
你自己的软件，无 copyleft 约束，详见 [LICENSE.MIT](LICENSE.MIT)。

守护进程（`daemon/`、`taskhealthd`）为 **GPL-2.0-or-later**，详见 [LICENSE](LICENSE)。

Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
Author: Lu Haoran <luhaoran@symthosm.com> 芦浩然
