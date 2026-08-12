# TaskHealth

Linux 用户态线程健康监控，填补 systemd（进程级）和 QNX HAM 在线程层面的空白。

**架构**：客户端-守护进程（Client-Daemon），灵感来源于 QNX HAM。
守护进程 `taskhealthd` 通过 Unix 域套接字接受多进程注册，统一执行心跳检测、
/proc 探针和告警输出；客户端库 `libtaskhealth` 链接到业务进程，通过 IPC 发送
注册/心跳/注销消息。

检测能力：线程意外退出、死锁（futex）、等锁超时。

## 依赖

- Linux 3.x+，需 `/proc` 文件系统
- GCC（C11），GNU Make
- 无外部库依赖

## 构建

```sh
make          # libtaskhealth.a + taskhealthd
make all-full # 库 + 守护进程 + demo + 单元测试
make check    # 编译并运行单元测试
make install  # 安装到 /usr/local
```

## 快速开始

```sh
# 终端 1：启动守护进程
taskhealthd -s /tmp/taskhealth.sock -i 500

# 终端 2：运行业务程序
LD_LIBRARY_PATH=. ./demo/demo
```

业务代码示例见 [demo/demo.c](demo/demo.c)，API 文档见 [src/taskhealth.h](src/taskhealth.h)。

## 文档

| 文档 | 说明 |
|------|------|
| [doc/DESIGN.md](doc/DESIGN.md) | 架构设计 |
| [doc/DETAILED_DESIGN.md](doc/DETAILED_DESIGN.md) | 详细设计 |
| [soft_copyright/用户手册.md](soft_copyright/用户手册.md) | 用户手册 |

## 打包

```sh
make deb  # Debian / Ubuntu
make rpm  # Fedora / RHEL
```

## 许可

GPL-2.0-or-later. 详见 [LICENSE](LICENSE).

Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
Author: Lu Haoran <luhaoran@symthosm.com> 芦浩然
