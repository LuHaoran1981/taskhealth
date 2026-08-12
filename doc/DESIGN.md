# TaskHealth 设计方案（守护进程架构）

## 1. 系统架构

```
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│     进程 A        │  │     进程 B        │  │     进程 C        │
│                  │  │                  │  │                  │
│  pthread_create  │  │  pthread_create  │  │  pthread_create  │
│  register()      │  │  register()      │  │  register()      │
│  heartbeat() ──────── heartbeat() ──────── heartbeat() ──────┐
│  unregister()    │  │  unregister()    │  │  unregister()    │  │
└──────────────────┘  └──────────────────┘  └──────────────────┘  │
                                                                  │
                                          Unix Domain Socket      │
                                          (SOCK_SEQPACKET)        │
                                                                  │
                                                        ┌─────────▼──────────┐
                                                        │   taskhealthd      │
                                                        │   守护进程          │
                                                        │                    │
                                                        │ ┌────────────────┐ │
                                                        │ │  线程注册表      │ │
                                                        │ │ pid|tid|name   │ │
                                                        │ │ gap|lock_hold  │ │
                                                        │ │ heartbeat_ns   │ │
                                                        │ └───────┬────────┘ │
                                                        │         │          │
                                                        │ ┌───────▼────────┐ │
                                                        │ │  Watchdog 线程  │ │
                                                        │ │  周期扫描        │ │
                                                        │ │  ├─ tgkill(pid, │ │
                                                        │ │  │   tid, 0)    │ │
                                                        │ │  ├─ /proc 探针  │ │
                                                        │ │  └─ 告警回调    │ │
                                                        │ └────────────────┘ │
                                                        └────────────────────┘
```

**关键改变：**
- 守护进程（taskhealthd）独立运行，监控所有进程中的已注册线程
- 业务进程通过 Unix domain socket 与守护进程通信
- 注册时提交 `(pid, tid, name, gap_ms, lock_hold_ms)`，守护进程据此做跨进程检测
- 心跳通过 IPC 发送，守护进程更新注册表中的时间戳

## 2. 组件划分

```
taskhealthd            ─ 守护进程（独立可执行文件）
  ├─ main()           启动、解析参数、daemonize
  ├─ server.c         监听 Unix socket，处理 client 请求
  ├─ registry.c       线程注册表管理（CRUD）
  ├─ watchdog.c       watchdog 主循环 + 检测逻辑
  ├─ probe.c          /proc 探针层（路径改为 /proc/<pid>/task/<tid>/...）
  └─ alert.c          告警回调（syslog / 脚本 / 自定义插件）

libtaskhealth.a       ─ 客户端库（链接到业务进程）
  ├─ taskhealth.h     公开 API
  ├─ taskhealth.c     client 端实现（IPC 通信 + TLS 缓存）
  └─ protocol.h       IPC 协议定义（client/server 共享）

taskhealth_mutex.h/c  ─ 配套互斥锁（不变，仍在业务进程内）
```

## 3. IPC 协议定义

### 3.1 传输层

| 属性 | 值 |
|------|-----|
| 协议 | Unix domain socket |
| 路径 | `/run/taskhealth.sock` |
| 类型 | `SOCK_SEQPACKET`（保序保边界，避免粘包分包） |
| 地址族 | `AF_UNIX` |
| 权限 | 仅同 UID 连接（SO_PEERCRED 校验） |

### 3.2 消息结构

每条消息 = **固定头** + **可变 body**，body 长度在头中声明。

```
┌──────────┬──────────┬──────────┬──────────┬─────────────────────┐
│  magic   │  type    │  _reserved│ body_len │       body          │
│ (2Byte)  │ (1Byte)  │  (1Byte)  │ (4Byte)  │  (0 ~ 4096 Byte)    │
└──────────┴──────────┴──────────┴──────────┴─────────────────────┘
  固定头 (8 Byte)                          ←── body_len ──→
```

**公共头定义：**

```c
#define TASKHEALTH_MSG_MAGIC  0x5448  /* "TH" */

enum taskhealth_msg_type {
    MSG_REGISTER   = 1,   /* client → daemon: 注册线程 */
    MSG_UNREGISTER = 2,   /* client → daemon: 注销线程 */
    MSG_HEARTBEAT  = 3,   /* client → daemon: 心跳 */
    MSG_SHUTDOWN   = 4,   /* client → daemon: 进程退出 */
    MSG_RESPONSE   = 5,   /* daemon → client: 回复 */
};

typedef struct {
    uint16_t magic;       /* 魔数 0x5448，帧头校验 */
    uint8_t  type;        /* enum taskhealth_msg_type */
    uint8_t  _reserved;
    uint32_t body_len;    /* body 长度（不含头），主机字节序 */
} taskhealth_msg_hdr_t;
```

### 3.3 消息定义

#### MSG_REGISTER (type=1) — 注册线程

```
方向：client → daemon
触发：taskhealth_register(name, gap_ms, lock_hold_ms)
```

```c
typedef struct {
    int32_t  pid;                   /*  4B  getpid() */
    int32_t  tid;                   /*  4B  gettid()，内核 TID */
    char     name[32];              /* 32B  线程名称，\0 结尾 */
    int64_t  gap_ms;                /*  8B  心跳超时阈值 (ms)，0=仅退出检测 */
    int64_t  lock_hold_ms;          /*  8B  等锁超时阈值 (ms)，0=不检测 */
} msg_body_register_t;              /* ───── 总计 56 Byte */
```

#### MSG_UNREGISTER (type=2) — 注销线程

```
方向：client → daemon
触发：taskhealth_unregister()
```

```c
typedef struct {
    int32_t  pid;                   /*  4B */
    int32_t  tid;                   /*  4B */
} msg_body_unregister_t;            /* ───── 总计 8 Byte */
```

#### MSG_HEARTBEAT (type=3) — 心跳

```
方向：client → daemon (单向，无回复)
触发：taskhealth_heartbeat()
频率：由业务线程调用频率决定，典型 10~100Hz
```

```c
typedef struct {
    int32_t  pid;                   /*  4B */
    int32_t  tid;                   /*  4B */
} msg_body_heartbeat_t;             /* ───── 总计 8 Byte */
```

#### MSG_SHUTDOWN (type=4) — 进程退出

```
方向：client → daemon (单向，无回复)
触发：taskhealth_shutdown() 或 client 进程 atexit
```

```c
typedef struct {
    int32_t  pid;                   /*  4B */
} msg_body_shutdown_t;              /* ───── 总计 4 Byte */
```

#### MSG_RESPONSE (type=5) — 回复

```
方向：daemon → client
触发：收到 MSG_REGISTER 后回复
```

```c
typedef struct {
    uint8_t  status;                /*  1B  见状态码定义 */
    uint8_t  _reserved[3];          /*  3B  对齐填充 */
} msg_body_response_t;              /* ───── 总计 4 Byte */
```

### 3.4 消息尺寸汇总

| 消息 | body 大小 | 全帧大小（含 8B 头） |
|------|:------:|:------:|
| MSG_REGISTER | 56 B | 64 B |
| MSG_UNREGISTER | 8 B | 16 B |
| MSG_HEARTBEAT | 8 B | 16 B |
| MSG_SHUTDOWN | 4 B | 12 B |
| MSG_RESPONSE | 4 B | 12 B |

### 3.5 状态码

```c
#define MSG_STATUS_OK             0x00  /* 成功 */
#define MSG_STATUS_TABLE_FULL     0x01  /* 注册表满 */
#define MSG_STATUS_ALREADY_REG    0x02  /* 已注册 */
#define MSG_STATUS_INVALID_ARG    0x03  /* 参数非法 */
#define MSG_STATUS_ERROR          0xFF  /* 内部错误 */
```

### 3.6 通信时序

```
Client (业务进程)                        Daemon (taskhealthd)
      │                                        │
      │──── ① connect() ──────────────────────→│  accept
      │                                        │  SO_PEERCRED 校验 UID
      │                                        │
      │──── ② MSG_REGISTER ───────────────────→│  registry_add()
      │                                        │  记录 (pid, tid, name, gap, hold)
      │←─── MSG_RESPONSE {status} ────────────│  回复 OK / TABLE_FULL / ...
      │                                        │
      │──── ③ MSG_HEARTBEAT {pid, tid} ──────→│  registry_heartbeat()
      │──── ③ MSG_HEARTBEAT {pid, tid} ──────→│     "   (one-way, 无回复)
      │──── ③ MSG_HEARTBEAT {pid, tid} ──────→│     "
      │     ...                                │
      │                                        │  watchdog 检测到异常
      │                                        │  syslog / 脚本告警
      │                                        │
      │──── ④ MSG_UNREGISTER ─────────────────→│  registry_remove()
      │                                        │
      │──── ⑤ MSG_SHUTDOWN ───────────────────→│  registry_cleanup_pid()
      │──── close() ───────────────────────────→│  (或 epoll 检测到 disconnect)
```

### 3.7 连接断开处理

```
Daemon 通过 epoll(EPOLLHUP) 检测 client 断开：

  client 正常 shutdown:
    ① 收到 MSG_SHUTDOWN → 清理该 pid 所有条目 → 期待 close
    ② 收到 close → epoll 清理

  client 异常崩溃（未调 shutdown）:
    ① epoll 检测到 EPOLLHUP，但没收到 SHUTDOWN
    ② 清理该 pid 所有注册条目（不触发 unexpected_exit 告警）

  daemon 崩溃:
    ① client send() 返回 EPIPE
    ② client 错误处理：记录日志，标记 initialized=false
    ③ 下次 register/heartbeat 尝试重连
```

### 3.8 字节对齐

所有协议 struct 显式声明 `__attribute__((packed))`，确保字段间无隐式填充。

```
msg_body_register_t 字段布局 (56B, packed):
  0    1    2    3    4    5    6    7
┌────┬────┬────┬────┬────┬────┬────┬────┐
│ pid (int32)    │ tid (int32)    │ name[0..7]   │
├────┴────┴────┴────┼────┴────┴────┼────┴────┴────┤
│ name[8..15]      │ name[16..23] │ name[24..31] │
├────┬────┬────┬────┼────┬────┬────┼────┬────┬────┤
│ gap_ms (int64)                 │ lock_hold_ms (int64)        │
└────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘
  ← 0-3 → ← 4-7 → ← 8-15 → ← 16-23 → ← 24-31 → ← 32-39 → ← 40-47 → ← 48-55 →

msg_body_heartbeat_t / msg_body_unregister_t 字段布局 (8B, packed):
  0    1    2    3    4    5    6    7
┌────┬────┬────┬────┬────┬────┬────┬────┐
│ pid (int32)    │ tid (int32)    │
└────┴────┴────┴────┴────┴────┴────┴────┘

msg_body_shutdown_t 字段布局 (4B, packed):
  0    1    2    3
┌────┬────┬────┬────┐
│ pid (int32)    │
└────┴────┴────┴────┘

msg_body_response_t 字段布局 (4B, packed):
  0    1    2    3
┌────┬────┬────┬────┐
│ status │ _reserved  │
└────┴────┴────┴────┘
```

**设计约束：**

```c
/* 编译时校验结构体尺寸 */
_Static_assert(sizeof(msg_body_register_t)  == 56, "register body size");
_Static_assert(sizeof(msg_body_heartbeat_t) ==  8, "heartbeat body size");
_Static_assert(sizeof(msg_body_unregister_t)==  8, "unregister body size");
_Static_assert(sizeof(msg_body_shutdown_t)  ==  4, "shutdown body size");
_Static_assert(sizeof(msg_body_response_t)  ==  4, "response body size");
_Static_assert(sizeof(taskhealth_msg_hdr_t) ==  8, "header size");
```

### 3.9 设计约束

| 约束 | 说明 |
|------|------|
| 字节对齐 | 所有消息 struct 加 `__attribute__((packed))` + 编译期 `_Static_assert` 校验 |
| 心跳单向 | MSG_HEARTBEAT 不等待回复，减少 RTT 开销 |
| 注册同步 | MSG_REGISTER 阻塞等待回复，确保注册结果可靠 |
| 魔数校验 | 头 2 字节固定 0x5448，过滤协议错误 |
| 字节序 | 整数使用主机字节序（同一主机通信，无需网络序） |
| body 上限 | 单条 body ≤ 4096 字节 |
| UID 校验 | accept 后通过 SO_PEERCRED 拒绝非同 UID 连接 |

## 4. 注册状态机

### 4.1 守护进程侧（Entry 生命周期）

```
                         ┌─────────────────────────────┐
                         │         FREE                  │
                         │  active=false, client_fd=-1   │
                         └──────────┬───────────────────┘
                                    │ MSG_REGISTER + 有空 slot
                                    ▼
                         ┌─────────────────────────────┐
                         │         ACTIVE               │
                         │  active=true, client_fd=fd   │
                         │  收到 heartbeat → 更新时间戳  │
                         └────┬────────────┬───────────┘
                              │            │
              MSG_UNREGISTER  │            │ client 断开 (EPOLLHUP)
              或 watchdog     │            │ 或 tgkill→ESRCH
              检测到线程退出    │            │
                              ▼            ▼
                         ┌─────────────────────────────┐
                         │         INACTIVE             │
                         │  active=false, client_fd=-1   │
                         │  不触发告警（主动清理）         │
                         └─────────────────────────────┘
```

**状态定义：**

| 状态 | active | client_fd | 说明 |
|------|:------:|:---------:|------|
| FREE | false | -1 | 空闲 slot，可被新注册占用 |
| ACTIVE | true | ≥0 | 正在监控，接受心跳、做检测 |
| INACTIVE | false | -1 | 已清理，slot 可复用 |

**状态转换触发：**

| 转换 | 触发条件 | 动作 |
|------|---------|------|
| FREE → ACTIVE | 收到 MSG_REGISTER | 填充 pid/tid/name/gap/lock，回复 OK |
| ACTIVE → INACTIVE | 收到 MSG_UNREGISTER | 清理 entry，回复 OK |
| ACTIVE → INACTIVE | watchdog 检测到 tgkill→ESRCH | 触发 `on_unexpected_exit` 告警 |
| ACTIVE → INACTIVE | epoll 检测到 client_fd 断开 | 批量清理该 pid 所有 entry（不告警） |
| INACTIVE → FREE | 任意时刻 | slot 开放给下一个 register |

### 4.2 客户端侧（线程 TLD 状态）

```
                         ┌─────────────────────────────┐
                         │        NOT_REGISTERED        │
                         │  TLS == NULL                 │
                         └──────────┬───────────────────┘
                                    │ taskhealth_register()
                                    ▼
                         ┌─────────────────────────────┐
                         │        REGISTERING           │
                         │  发送 MSG_REGISTER → 等回复   │
                         └────┬────────────────────┬───┘
                              │                    │
              daemon 回复 OK  │                    │ daemon 回复 ERR_*
                              ▼                    ▼
                         ┌─────────────────────┐ ┌─────────────────────────┐
                         │     REGISTERED      │ │    NOT_REGISTERED       │
                         │  TLS == (void*)1    │ │  (返回错误码给调用方)      │
                         │  可调 heartbeat()    │ └─────────────────────────┘
                         └────────┬────────────┘
                                  │ taskhealth_unregister()
                                  ▼
                         ┌─────────────────────────────┐
                         │        NOT_REGISTERED        │
                         │  TLS == NULL                 │
                         └─────────────────────────────┘
```

**客户端不存储 entry index**（与进程内库不同），TLS 仅记录 "本线程是否已注册" 的单比特标记（NULL / (void*)1）。注册数据全部在守护进程。

## 5. 守护进程设计

### 5.1 启动与生命周期

```
main():
  1. 解析命令行参数（-c config, -d daemonize）
  2. 可选 daemonize（fork, setsid）
  3. 创建并绑定 /run/taskhealth.sock
  4. 初始化注册表（固定大小，默认 4096 条目）
  5. 启动 watchdog 线程
  6. 进入 accept 循环
  7. 收到 SIGTERM → 优雅关闭
```

### 5.2 守护进程配置

```c
struct taskhealthd_config {
    char        socket_path[256];   // socket 路径，默认 /run/taskhealth.sock
    int64_t     check_interval_ms;  // watchdog 扫描间隔，默认 1000
    int         max_entries;        // 注册表容量，默认 4096
    bool        daemonize;          // 是否后台化
    char        log_file[256];      // 日志文件，空 = syslog
    char        alert_script[256];  // 告警时执行的脚本，空 = 不执行
};
```

### 5.3 线程注册表条目

```c
typedef struct {
    pid_t           pid;                    // 进程 PID（来自 client 注册）
    pid_t           tid;                    // 内核 TID（来自 client 注册）
    int             client_fd;              // 对应 client 的 socket fd（-1=已断开）
    char            name[TASKHEALTH_NAME_LEN];
    int64_t         gap_ms;
    int64_t         lock_hold_ms;
    int64_t         last_heartbeat_ns;      // CLOCK_MONOTONIC
    int64_t         lock_wait_start_ns;
    bool            active;
    bool            alerted_exit;
    bool            alerted_deadlock;
    bool            alerted_lock_wait;
} Entry;
```

### 5.4 Watchdog 检测流程

Watchdog 对每个 ACTIVE 条目执行检测。检测分两层：

- **第一层**：`tgkill` 退出检测，始终运行
- **第二层**：心跳超时后才触发 /proc 探查，再做死锁和等锁超时判定

```
while (running):
    for each active entry:
        now = clock_gettime(CLOCK_MONOTONIC)

        // ① 退出检测（始终运行）
        ret = tgkill(entry.pid, entry.tid, 0)
        if ret == -1 && errno == ESRCH:
            alert("unexpected_exit", entry)
            entry → INACTIVE
            continue  // 线程不存在，跳过后续

        // ② 心跳超时检测（仅 gap_ms > 0 时激活）
        if entry.gap_ms > 0:
            elapsed = now - entry.last_heartbeat_ns
            if elapsed > entry.gap_ms * 1000000:  // ms → ns

                // 心跳超时 → 一次性读取 /proc
                state = read_proc_status(entry.pid, entry.tid)
                wchan = read_proc_wchan(entry.pid, entry.tid)

                if (state == 'S' || state == 'D') && strstr(wchan, "futex"):

                    // 线程卡在 futex 上 → 读取 futex 地址 + 解析模块
                    futex_addr = read_proc_syscall(entry.pid, entry.tid)
                    module = resolve_maps(entry.pid, futex_addr)

                    // ②a 死锁判定（gap_ms 触发的超时即判定为死锁）
                    alert("deadlock", entry, wchan, futex_addr, module)

                    // ②b 等锁超时（lock_hold_ms > 0 时累计等待时长）
                    if entry.lock_hold_ms > 0:
                        if entry.lock_wait_start_ns == 0:
                            entry.lock_wait_start_ns = now
                        waited = now - entry.lock_wait_start_ns
                        if waited > entry.lock_hold_ms * 1000000:
                            alert("lock_wait_timeout", entry, waited,
                                  futex_addr, module)
                else:
                    // 不在 futex 上，重置等锁计时
                    entry.lock_wait_start_ns = 0

    nanosleep(check_interval)
```

**检测逻辑总结：**

```
tgkill(pid, tid, 0)
  ├─ ESRCH  →  "unexpected exit" 告警，entry → INACTIVE
  └─ 0 (存活)
       └─ gap_ms > 0 && 心跳超时?
            ├─ 否 → 跳过
            └─ 是 → 读 /proc (status + wchan)
                  ├─ 不在 futex → 重置 lock_wait_start_ns
                  └─ 在 futex 上
                       ├─ 告警 "deadlock"
                       └─ lock_hold_ms > 0?
                            ├─ 累计等待时间
                            └─ 超时 → 告警 "lock_wait_timeout"
```

**参数组合场景：**

| gap_ms | lock_hold_ms | 行为 |
|:------:|:------------:|------|
| 0 | 任意 | 仅退出检测，不读 /proc |
| >0 | 0 | 退出检测 + 死锁告警（不关心等锁时长） |
| >0 | >0 | 退出检测 + 死锁告警 + 等锁超时告警 |

**关键差异（与进程内库对比）：**

| 项目            | 进程内库                    | 守护进程 |
|----------------|-----------------------------|---------|
| 退出检测        | `tgkill(getpid(), tid, 0)`  | `tgkill(pid, tid, 0)` |
| /proc 路径     | `/proc/self/task/<tid>/`    | `/proc/<pid>/task/<tid>/` |
| 心跳时间戳     | `_Atomic` 变量，当前进程写    | `int64_t` 普通变量，守护进程更新 |
| client_fd 追踪 | 不需要                       | 需要，检测 client 断开 |

### 5.5 client 断开检测

```
守护进程在 accept 循环中监控 client_fd：
  - 正常 unregister → 清理条目
  - client 进程崩溃 → epoll 检测到 EOF/hangup
    → 将该进程的所有条目标记 inactive
    → 不为已断开的 client 触发 unexpected_exit（避免误报）
```

## 6. 客户端库设计

### 6.1 API（与之前兼容）

```c
const char *taskhealth_version(void);
int  taskhealth_init(const struct taskhealth_config *cfg);
void taskhealth_shutdown(void);
int  taskhealth_register(const char *name, int64_t gap_ms, int64_t lock_hold_ms);
void taskhealth_unregister(void);
void taskhealth_heartbeat(void);
```

**register 参数规则：**

| 参数 | 传参 | 行为 |
|------|------|------|
| `name` | `"render-thread"` | 使用用户指定的名称 |
| `name` | `NULL` 或 `""` | 自动生成：读取 `/proc/self/comm` 取可执行文件名，拼接内核 TID → `"myapp.5678"` |
| `gap_ms` | `0` | 仅做退出检测，不做心跳超时和死锁检测（/proc 零开销） |
| `gap_ms` | `>0` | 心跳超时阈值，超时后触发 /proc 探查和死锁判定 |
| `lock_hold_ms` | `0` | 不检测等锁超时 |
| `lock_hold_ms` | `>0` | 等锁超时阈值，在心跳已超时的前提下累计等待时长 |

```c
// 客户端命名逻辑（伪代码）
if (name == NULL || name[0] == '\0') {
    char comm[32];
    FILE *f = fopen("/proc/self/comm", "r");
    fgets(comm, sizeof(comm), f);
    fclose(f);
    comm[strcspn(comm, "\n")] = '\0';
    snprintf(auto_name, sizeof(auto_name), "%s.%d", comm, (int)gettid());
    name = auto_name;   // "myapp.5678"
}
```

### 6.2 内部实现

```
taskhealth_init(cfg):
  1. 连接 /run/taskhealth.sock（或 cfg 指定的路径）
  2. 保存 socket fd 到全局状态
  3. 启动内部心跳定时器线程（可选优化：合并心跳）

taskhealth_register(name, gap_ms, lock_hold_ms):
  1. name 为 NULL/空 → 自动生成（读 /proc/self/comm + tid）
  2. 获取 pid = getpid(), tid = gettid()
  3. 构造 MSG_REGISTER 消息 {pid, tid, name, gap_ms, lock_hold_ms}
  4. 发送到守护进程
  5. TLS 记录已注册状态

taskhealth_heartbeat():
  1. 发送 MSG_HEARTBEAT {pid, tid}
  2. （可选优化：由库内部定时器批量发送）

taskhealth_unregister():
  1. 发送 MSG_UNREGISTER {pid, tid}
  2. 清除 TLS 标记

taskhealth_shutdown():
  1. 发送 MSG_SHUTDOWN {pid}
  2. 关闭 socket
```

### 6.3 心跳优化策略

```
方案 A（默认）：每次 heartbeat() 调用都发 IPC
  - 优点：实时，守护进程看到的延迟 < 一次 IPC RTT (~µs)
  - 缺点：高频调用时（每 1ms 一次），IPC 频率过高

方案 B（可选）：客户端本地定时器 100ms 批量发送
  - 优点：减少 IPC 次数，适合 >100Hz 的心跳
  - 缺点：守护进程看到的心跳粒度变粗

默认选择方案 A，对绝大多数场景足够。
```

## 7. /proc 探针路径变化

守护进程通过 `/proc/<pid>/task/<tid>/` 路径跨进程读取线程信息：

| 文件 | 内容 | 用途 |
|------|------|------|
| `/proc/<pid>/task/<tid>/status` | `State:\tS` 行 | 获取线程状态（R/S/D/T/Z） |
| `/proc/<pid>/task/<tid>/wchan` | `futex_wait_queue_me` 等 | 判断阻塞原因 |
| `/proc/<pid>/task/<tid>/syscall` | `0 0x0 0x<futex_addr> ...` | 读取 futex 等待地址 |
| `/proc/<pid>/maps` | `7f...-7f... rw-p ... /usr/lib/libxxx.so` | 将 futex 地址解析为模块+偏移 |

**与进程内库的路径对比：**

| 场景 | 进程内库 | 守护进程 |
|------|---------|---------|
| 线程状态 | `/proc/self/task/<tid>/status` | `/proc/<pid>/task/<tid>/status` |
| 等待通道 | `/proc/self/task/<tid>/wchan` | `/proc/<pid>/task/<tid>/wchan` |
| 系统调用 | `/proc/self/task/<tid>/syscall` | `/proc/<pid>/task/<tid>/syscall` |
| 内存映射 | `/proc/self/maps` | `/proc/<pid>/maps` |

> 守护进程需要同时提供 `pid` 和 `tid` 才能定位目标线程。

## 8. 告警回调

守护进程内回调（默认按优先级尝试）：

```
  1. syslog  → 写入系统日志（journald / syslog）
  2. stderr  → 前台运行时输出
  3. 外部脚本 → 执行 /etc/taskhealth/alert.d/*.sh
              传入环境变量: TASKHEALTH_TYPE, TASKHEALTH_NAME,
                            TASKHEALTH_PID, TASKHEALTH_TID,
                            TASKHEALTH_MODULE
```

## 9. 配套互斥锁

不变——`taskhealth_mutex_t` 仍在业务进程内使用，锁名解析改在守护进程侧。守护进程通过 futex 地址 + 目标进程的 `/proc/<pid>/maps` 解析模块路径。

> 锁名解析不再依赖 `taskhealth_mutex_resolve()`（跨进程后地址空间不同，无法直接比对地址）。替代方案：守护进程解析 `/proc/<pid>/maps` 获取模块名 + 偏移，用户通过偏移在源码中定位锁变量。

## 10. 项目结构

```
TaskHealth/
├── daemon/                  # 守护进程源码
│   ├── main.c
│   ├── server.c
│   ├── registry.c
│   ├── registry.h
│   ├── watchdog.c
│   ├── watchdog.h
│   ├── probe.c
│   ├── probe.h
│   ├── alert.c
│   └── alert.h
├── src/                     # 客户端库
│   ├── taskhealth.h
│   ├── taskhealth.c
│   ├── taskhealth_mutex.h
│   ├── taskhealth_mutex.c
│   └── protocol.h           # IPC 协议（client/server 共享）
├── test/                    # 单元测试
├── demo/                    # 演示程序
├── systemd/                 # systemd service 文件
│   └── taskhealthd.service
├── debian/
├── contrib/
│   ├── rpm/
│   └── yocto/
├── doc/
├── Makefile
└── README.md
```

## 11. 关键技术决策

| 决策 | 理由 |
|------|------|
| Unix domain socket 而非共享内存 | 简单、可靠、自带连接追踪（client 断开可检测） |
| SOCK_SEQPACKET 而非 SOCK_STREAM | 避免粘包，消息天然边界 |
| tgkill(pid, tid, 0) 跨进程检查存活 | 同一系统调用，只需换 pid 参数 |
| /proc/<pid>/task/<tid> 路径 | 守护进程无特权即可读（同 UID） |
| 守护进程只接受同 UID 连接 | 通过 `SO_PEERCRED` 校验，安全边界 |
| 告警在守护进程侧执行 | 业务进程可能已死，回调必须在 daemon 中 |
| taskhealth_mutex_resolve 废除 | 跨进程后虚地址空间不同，由模块+偏移定位锁 |

---

Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
上海先道智觉科技有限责任公司 | Author: Lu Haoran 芦浩然
SPDX-License-Identifier: GPL-2.0-or-later
