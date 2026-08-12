# TaskHealth 详细设计（守护进程架构）

## 1. 模块分解

```
daemon/
  main.c         ─ 入口：参数解析、daemonize、信号处理、启动 server+watchdog
  server.c       ─ IPC 服务：accept 连接、收/发消息、dispatch 到 registry
  registry.c/h   ─ 注册表：CRUD、查找、清理（client 断开时批量失效）
  watchdog.c/h   ─ 检测引擎：三阶段扫描循环
  probe.c/h      ─ /proc 探针：状态/wchan/syscall/maps，路径带 pid 参数
  alert.c/h      ─ 告警模块：syslog、脚本执行、日志文件

src/
  taskhealth.h    ─ 公开 API（兼容原有接口，内部改为 IPC 通信）
  taskhealth.c    ─ client 端实现（socket + TLS 缓存）
  protocol.h     ─ IPC 消息定义（client/server 共享）
```

## 2. IPC 协议详细设计

### 2.1 连接生命周期

```
Client (业务进程)                         Daemon (taskhealthd)
      │                                         │
      │── connect("/run/taskhealth.sock") ─────→│  accept
      │                                         │  SO_PEERCRED 校验 UID
      │                                         │
      │── MSG_REGISTER ────────────────────────→│  写入注册表
      │                                         │
      │── MSG_HEARTBEAT ───────────────────────→│  更新 last_heartbeat_ns
      │── MSG_HEARTBEAT ───────────────────────→│  ...
      │                                         │
      │── MSG_UNREGISTER ──────────────────────→│  清理注册表
      │                                         │
      │── close() ─────────────────────────────→│  epoll 检测到断开
      │                                         │  清理该 pid 所有条目
```

### 2.2 消息格式

**公共头（8 字节）：**

```c
#define TASKHEALTH_MSG_MAGIC  0x5448  /* "TH" */

typedef struct {
    uint16_t magic;       /* 魔数 0x5448，帧头校验 */
    uint8_t  type;        /* enum taskhealth_msg_type */
    uint8_t  _reserved;
    uint32_t body_len;    /* body 长度（不含头），主机字节序 */
} __attribute__((packed)) taskhealth_msg_hdr_t;
```

**消息类型：**

```c
enum taskhealth_msg_type {
    MSG_REGISTER   = 1,   /* client → daemon: 注册线程 */
    MSG_UNREGISTER = 2,   /* client → daemon: 注销线程 */
    MSG_HEARTBEAT  = 3,   /* client → daemon: 心跳 */
    MSG_SHUTDOWN   = 4,   /* client → daemon: 进程退出 */
    MSG_RESPONSE   = 5,   /* daemon → client: 回复 */
};
```

**各消息 body 定义：**

```c
/* MSG_REGISTER — 注册线程 (56B) */
typedef struct {
    int32_t  pid;                   /*  4B  getpid() */
    int32_t  tid;                   /*  4B  gettid() */
    char     name[32];              /* 32B  线程名称，\0 结尾 */
    int64_t  gap_ms;                /*  8B  心跳超时阈值 (ms)，0=仅退出检测 */
    int64_t  lock_hold_ms;          /*  8B  等锁超时阈值 (ms)，0=不检测 */
} __attribute__((packed)) msg_body_register_t;

/* MSG_UNREGISTER — 注销线程 (8B) */
typedef struct {
    int32_t  pid;                   /*  4B */
    int32_t  tid;                   /*  4B */
} __attribute__((packed)) msg_body_unregister_t;

/* MSG_HEARTBEAT — 心跳 (8B) */
typedef struct {
    int32_t  pid;                   /*  4B */
    int32_t  tid;                   /*  4B */
} __attribute__((packed)) msg_body_heartbeat_t;

/* MSG_SHUTDOWN — 进程退出 (4B) */
typedef struct {
    int32_t  pid;                   /*  4B */
} __attribute__((packed)) msg_body_shutdown_t;

/* MSG_RESPONSE — 回复 (4B) */
typedef struct {
    uint8_t  status;                /*  1B  见状态码定义 */
    uint8_t  _reserved[3];          /*  3B  对齐填充 */
} __attribute__((packed)) msg_body_response_t;
```

**编译期校验：**

```c
_Static_assert(sizeof(msg_body_register_t)   == 56, "register body size");
_Static_assert(sizeof(msg_body_heartbeat_t)  ==  8, "heartbeat body size");
_Static_assert(sizeof(msg_body_unregister_t) ==  8, "unregister body size");
_Static_assert(sizeof(msg_body_shutdown_t)   ==  4, "shutdown body size");
_Static_assert(sizeof(msg_body_response_t)   ==  4, "response body size");
_Static_assert(sizeof(taskhealth_msg_hdr_t)  ==  8, "header size");
```

**消息尺寸汇总：**

| 消息 | body 大小 | 全帧大小（含 8B 头） |
|------|:------:|:------:|
| MSG_REGISTER | 56 B | 64 B |
| MSG_UNREGISTER | 8 B | 16 B |
| MSG_HEARTBEAT | 8 B | 16 B |
| MSG_SHUTDOWN | 4 B | 12 B |
| MSG_RESPONSE | 4 B | 12 B |

### 2.3 状态码与回复

守护进程对 MSG_REGISTER 回复 MSG_RESPONSE 消息（4 字节 body）：

```c
#define MSG_STATUS_OK             0x00  /* 成功 */
#define MSG_STATUS_TABLE_FULL     0x01  /* 注册表满 */
#define MSG_STATUS_ALREADY_REG    0x02  /* 已注册 (同一 tid) */
#define MSG_STATUS_INVALID_ARG    0x03  /* 参数非法 (gap_ms<0 等) */
#define MSG_STATUS_ERROR          0xFF  /* 内部错误 */
```

MSG_HEARTBEAT、MSG_UNREGISTER、MSG_SHUTDOWN 为单向消息，无回复。

## 3. 守护进程详细设计

### 3.1 全局状态

```c
static struct {
    struct taskhealthd_config cfg;
    int              listen_fd;
    int              epoll_fd;
    Entry           *entries;
    int              entry_capacity;
    int              entry_count;
    pthread_t        watchdog_tid;
    bool             running;
    pthread_mutex_t  registry_lock;
} g_daemon;
```

### 3.2 注册表条目

```c
#define TASKHEALTH_NAME_LEN  32

typedef struct {
    int32_t  pid;
    int32_t  tid;
    int      client_fd;             /* -1 = 空闲，≥0 = 对应 socket */
    char     name[TASKHEALTH_NAME_LEN];
    int64_t  gap_ms;
    int64_t  lock_hold_ms;

    int64_t  last_heartbeat_ns;     /* daemon 收到 MSG_HEARTBEAT 时更新 */
    int64_t  lock_wait_start_ns;    /* watchdog 线程专用 */
    bool     active;
    bool     alerted_exit;
    bool     alerted_deadlock;
    bool     alerted_lock_wait;
} Entry;
```

### 3.3 server.c — accept 循环

```
epoll 事件循环:
  events = epoll_wait(epoll_fd, ...)

  for each event:
    if fd == listen_fd:
      client_fd = accept(listen_fd, ...)
      getsockopt(SO_PEERCRED, &ucred)    /* 获取 client 的 pid/uid */
      if ucred.uid != getuid():          /* 只允许同 UID */
        close(client_fd)
        continue
      epoll_ctl(EPOLL_CTL_ADD, client_fd, ...)

    else if event & EPOLLHUP:            /* client 断开 */
      registry_cleanup_pid(fd_to_pid[fd])
      close(fd)

    else if event & EPOLLIN:             /* 收到消息 */
      recv(client_fd, buf, sizeof(buf), MSG_PEEK)
      hdr = (msg_hdr_t*)buf
      switch hdr.type:
        MSG_REGISTER   → registry_add(hdr, body)
        MSG_HEARTBEAT  → registry_heartbeat(body.pid, body.tid)
        MSG_UNREGISTER → registry_remove(body.pid, body.tid)
        MSG_SHUTDOWN   → registry_cleanup_pid(body.pid)
```

### 3.4 watchdog.c — 主循环

```c
static void *watchdog_thread(void *arg)
{
    (void)arg;
    int i;

    while (g_daemon.running) {
        int64_t loop_start = now_ns();
        int64_t now;
        int64_t elapsed, sleep_ns;

        pthread_mutex_lock(&g_daemon.registry_lock);
        for (i = 0; i < g_daemon.entry_capacity; i++) {
            Entry *e = &g_daemon.entries[i];

            if (!e->active)
                continue;

            /* ① 退出检测（始终运行） */
            int alive = thread_alive(e->pid, e->tid);
            if (alive == 0) {
                handle_exit(e);
                continue;
            }
            if (alive < 0)
                continue;

            /* ② 心跳超时检测（仅 gap_ms > 0 时激活） */
            if (e->gap_ms > 0) {
                now = now_ns();
                if (check_heartbeat_timeout(e, now)) {
                    /* 心跳超时 → 读 /proc，同时做死锁和等锁超时判定 */
                    probe_and_detect(e, now);
                }
            }
        }
        pthread_mutex_unlock(&g_daemon.registry_lock);

        elapsed  = now_ns() - loop_start;
        sleep_ns = g_daemon.cfg.check_interval_ms * 1000000LL - elapsed;
        if (sleep_ns > 0) {
            struct timespec ts = {
                .tv_sec  = sleep_ns / 1000000000LL,
                .tv_nsec = sleep_ns % 1000000000LL,
            };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}
```

### 3.5 probe.c — 检测函数

**心跳超时判定（纯时间计算，不读 /proc）：**

```c
static bool check_heartbeat_timeout(Entry *e, int64_t now)
{
    int64_t elapsed = now - e->last_heartbeat_ns;
    return elapsed > e->gap_ms * 1000000LL;
}
```

**心跳超时后的联合检测（一次 /proc 读取，双重判定）：**

```c
static void probe_and_detect(Entry *e, int64_t now)
{
    /* 一次性读取 /proc */
    char state = read_thread_state(e->pid, e->tid);
    char wchan[64];
    bool got_wchan = read_wchan(e->pid, e->tid, wchan, sizeof(wchan));

    /* 不在 futex 上 → 不是死锁/等锁，重置计时 */
    if (!got_wchan || !strstr(wchan, "futex")) {
        e->lock_wait_start_ns = 0;
        return;
    }
    if (state != 'S' && state != 'D') {
        e->lock_wait_start_ns = 0;
        return;
    }

    /* 线程卡在 futex 上 → 读取 futex 地址 + 解析模块 */
    uintptr_t futex_addr = read_futex_addr(e->pid, e->tid);
    char *module = futex_addr ? resolve_futex_address(e->pid, futex_addr) : NULL;

    /* ②a 死锁判定（心跳超时 + futex 阻塞 = 死锁） */
    alert(ALERT_DEADLOCK, e, futex_addr, wchan, 0, module);

    /* ②b 等锁超时（lock_hold_ms > 0 时累计等待时长） */
    if (e->lock_hold_ms > 0) {
        if (e->lock_wait_start_ns == 0)
            e->lock_wait_start_ns = now;
        int64_t waited = now - e->lock_wait_start_ns;
        if (waited > e->lock_hold_ms * 1000000LL) {
            alert(ALERT_LOCK_WAIT, e, futex_addr, wchan,
                  waited / 1000000LL, module);
        }
    }
}
```

**决策树：**

```
tgkill(pid, tid, 0)
  ├─ ESRCh   →  handle_exit()        // 线程退出
  └─ 存活
       └─ gap_ms > 0 && 心跳超时?
            ├─ 否 → 跳过
            └─ 是 → 读 /proc status + wchan
                  ├─ 不在 futex → lock_wait_start_ns = 0，跳过
                  └─ 在 futex 且 state∈{S,D}
                       ├─ alert(DEADLOCK)
                       └─ lock_hold_ms > 0?
                            ├─ 累计 waited = now - lock_wait_start_ns
                            └─ waited > lock_hold_ms → alert(LOCK_WAIT)
```

**/proc 探针函数（跨进程路径）：**

```c
/* 线程存活检查 — 跨进程 tgkill */
static int thread_alive(pid_t pid, pid_t tid)
{
    long ret = syscall(SYS_tgkill, pid, tid, 0);
    if (ret == 0)
        return 1;          /* 存活 */
    if (errno == ESRCH)
        return 0;          /* 已退出 */
    return -1;             /* 其他错误 */
}

/* /proc/<pid>/task/<tid>/status — 读取线程状态 */
static char read_thread_state(pid_t pid, pid_t tid)
{
    char path[64];
    snprintf(path, sizeof(path),
             "/proc/%d/task/%d/status", (int)pid, (int)tid);
    FILE *f = fopen(path, "r");
    if (!f) return '?';

    char state = '?';
    char line[128];
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

/* /proc/<pid>/task/<tid>/wchan — 读取等待通道 */
static int read_wchan(pid_t pid, pid_t tid, char *buf, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path),
             "/proc/%d/task/%d/wchan", (int)pid, (int)tid);
    FILE *f = fopen(path, "r");
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

/* /proc/<pid>/task/<tid>/syscall — 读取 futex 地址 */
static uintptr_t read_futex_addr(pid_t pid, pid_t tid)
{
    char path[64];
    snprintf(path, sizeof(path),
             "/proc/%d/task/%d/syscall", (int)pid, (int)tid);
    /* 其余逻辑同进程内版本 */

    /* ... 解析 syscall nr == __NR_futex，提取 arg1 ... */
    return arg1;
}

/* /proc/<pid>/maps — 解析 futex 地址到模块 */
static char *resolve_futex_address(pid_t pid, uintptr_t addr)
{
    if (addr == 0) return NULL;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    /* 其余逻辑同进程内版本 */

    /* ... 遍历 maps，匹配地址范围 ... */
    /* 返回 "path+0xoffset" */
    return result;
}
```

**路径对照表：**

| 探针 | 进程内库 (`/proc/self/...`) | 守护进程 (`/proc/<pid>/...`) |
|------|---------------------------|---------------------------|
| status | `/proc/self/task/<tid>/status` | `/proc/<pid>/task/<tid>/status` |
| wchan | `/proc/self/task/<tid>/wchan` | `/proc/<pid>/task/<tid>/wchan` |
| syscall | `/proc/self/task/<tid>/syscall` | `/proc/<pid>/task/<tid>/syscall` |
| maps | `/proc/self/maps` | `/proc/<pid>/maps` |

### 3.6 alert.c — 告警模块

```c
/* 告警优先级：syslog → stderr → 外部脚本 */
static void alert(enum alert_type type, Entry *e,
                  uintptr_t futex_addr, const char *wchan,
                  int64_t wait_ms, const char *futex_module)
{
    char buf[512];

    switch (type) {
    case ALERT_EXIT:
        snprintf(buf, sizeof(buf),
                 "UNEXPECTED EXIT: thread '%s' pid=%d tid=%d",
                 e->name, (int)e->pid, (int)e->tid);
        break;
    case ALERT_DEADLOCK:
        snprintf(buf, sizeof(buf),
                 "DEADLOCK: thread '%s' pid=%d tid=%d "
                 "wchan=%s futex=0x%lx module=%s",
                 e->name, (int)e->pid, (int)e->tid,
                 wchan, (unsigned long)futex_addr,
                 futex_module ? futex_module : "?");
        break;
    case ALERT_LOCK_WAIT:
        snprintf(buf, sizeof(buf),
                 "LOCK WAIT TIMEOUT: thread '%s' pid=%d tid=%d "
                 "wait=%ldms futex=0x%lx module=%s",
                 e->name, (int)e->pid, (int)e->tid,
                 (long)wait_ms, (unsigned long)futex_addr,
                 futex_module ? futex_module : "?");
        break;
    }

    /* 1. syslog */
    syslog(LOG_ERR, "[TaskHealth] %s", buf);

    /* 2. stderr */
    fprintf(stderr, "[TaskHealth] %s\n", buf);

    /* 3. 外部脚本 */
    if (g_daemon.cfg.alert_script[0]) {
        pid_t child = fork();
        if (child == 0) {
            setenv("TASKHEALTH_TYPE", alert_type_name, 1);
            setenv("TASKHEALTH_NAME", e->name, 1);
            setenv("TASKHEALTH_PID", pid_str, 1);
            setenv("TASKHEALTH_TID", tid_str, 1);
            setenv("TASKHEALTH_MODULE", futex_module ? futex_module : "", 1);
            execl(g_daemon.cfg.alert_script,
                  g_daemon.cfg.alert_script, NULL);
            _exit(1);
        }
        /* 父进程不 wait，避免阻塞 watchdog */
    }
}
```

### 3.7 client 断连检测与清理

```
epoll 检测到 EPOLLHUP：
  1. 获取该 fd 对应的 pid（通过 fd→pid 映射表）
  2. registry_cleanup_pid(pid)：
     - 遍历 entries，找到所有 pid == 目标 && active 的条目
     - 设置 active = false, client_fd = -1
     - 不触发 unexpected_exit（client 进程退出是正常行为）
  3. close(fd), 清理映射表
```

> **设计要点：** client 进程正常退出时不触发 unexpected_exit，避免误报。只有 client 进程还活着但某个线程突然消失（tgkill 返回 ESRCH），才是真正的 unexpected exit。

## 4. 客户端库详细设计

### 4.1 全局状态

```c
static struct {
    int         sock_fd;        /* 到 daemon 的 socket */
    pid_t       pid;            /* 本地 PID，init 时缓存 */
    bool        initialized;
    pthread_key_t tls_key;      /* 值为 (void*)1 表示已注册 */
} g_client;
```

### 4.2 内部辅助函数

```c
/* 构造消息头 */
static inline taskhealth_msg_hdr_t build_hdr(uint8_t type, uint32_t body_len)
{
    return (taskhealth_msg_hdr_t){
        .magic    = TASKHEALTH_MSG_MAGIC,
        .type     = type,
        .body_len = body_len,
    };
}

/* 发送 header + body（原子发送，SEQPACKET 保证一次性到达） */
static int send_msg(int fd, taskhealth_msg_hdr_t *hdr, const void *body)
{
    struct iovec iov[2] = {
        { .iov_base = hdr,  .iov_len = sizeof(*hdr) },
        { .iov_base = (void *)body, .iov_len = hdr->body_len },
    };
    struct msghdr msg = {
        .msg_iov    = iov,
        .msg_iovlen = 2,
    };
    return sendmsg(fd, &msg, MSG_NOSIGNAL);
}

/* 精确接收指定字节数 */
static int recv_exact(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, (char *)buf + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}
```

### 4.3 关键实现

```c
const char *taskhealth_version(void)
{
    return TASKHEALTH_VERSION;
}

int taskhealth_init(const struct taskhealth_config *cfg)
{
    struct sockaddr_un addr;
    const char *path;

    if (g_client.initialized)
        return TASKHEALTH_ERR_ALREADY_INIT;

    path = (cfg && cfg->socket_path[0])
           ? cfg->socket_path : "/run/taskhealth.sock";

    g_client.sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (g_client.sock_fd < 0)
        return TASKHEALTH_ERR_NOT_INIT;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(g_client.sock_fd, (struct sockaddr *)&addr,
                sizeof(addr)) < 0) {
        close(g_client.sock_fd);
        return TASKHEALTH_ERR_NOT_INIT;
    }

    g_client.pid = getpid();
    pthread_key_create(&g_client.tls_key, NULL);
    g_client.initialized = true;
    return TASKHEALTH_OK;
}

void taskhealth_shutdown(void)
{
    if (!g_client.initialized)
        return;

    taskhealth_msg_hdr_t hdr = { .magic = TASKHEALTH_MSG_MAGIC,
                                 .type  = MSG_SHUTDOWN,
                                 .body_len = sizeof(msg_body_shutdown_t) };
    msg_body_shutdown_t body = { .pid = g_client.pid };

    send_msg(g_client.sock_fd, &hdr, &body);
    close(g_client.sock_fd);
    pthread_key_delete(g_client.tls_key);
    g_client.initialized = false;
}

int taskhealth_register(const char *name, int64_t gap_ms, int64_t lock_hold_ms)
{
    char auto_name[32];

    if (!g_client.initialized)
        return TASKHEALTH_ERR_NOT_INIT;
    if (gap_ms < 0 || lock_hold_ms < 0)
        return TASKHEALTH_ERR_INVALID_ARG;
    if (pthread_getspecific(g_client.tls_key))
        return TASKHEALTH_ERR_ALREADY_REG;

    /* 自动命名：name 为 NULL 或空字符串时，读取 /proc/self/comm + TID */
    if (!name || name[0] == '\0') {
        char comm[32];
        FILE *f = fopen("/proc/self/comm", "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f)) {
                comm[strcspn(comm, "\n")] = '\0';
                snprintf(auto_name, sizeof(auto_name), "%s.%d",
                         comm, (int)syscall(SYS_gettid));
            }
            fclose(f);
        }
        if (auto_name[0] == '\0')
            snprintf(auto_name, sizeof(auto_name), "tid.%d",
                     (int)syscall(SYS_gettid));
        name = auto_name;
    }

    msg_body_register_t body = {
        .pid = g_client.pid,
        .tid = (int32_t)syscall(SYS_gettid),
        .gap_ms = gap_ms,
        .lock_hold_ms = lock_hold_ms,
    };
    strncpy(body.name, name, sizeof(body.name) - 1);
    body.name[sizeof(body.name) - 1] = '\0';

    taskhealth_msg_hdr_t hdr = build_hdr(MSG_REGISTER, sizeof(body));
    send_msg(g_client.sock_fd, &hdr, &body);

    /* 等待 daemon 回复 MSG_RESPONSE */
    msg_body_response_t resp;
    recv_exact(g_client.sock_fd, &resp, sizeof(resp));

    if (resp.status != MSG_STATUS_OK)
        return daemon_status_to_error(resp.status);

    pthread_setspecific(g_client.tls_key, (void *)1);
    return TASKHEALTH_OK;
}

void taskhealth_unregister(void)
{
    if (!pthread_getspecific(g_client.tls_key))
        return;

    msg_body_unregister_t body = {
        .pid = g_client.pid,
        .tid = (int32_t)syscall(SYS_gettid),
    };

    taskhealth_msg_hdr_t hdr = build_hdr(MSG_UNREGISTER, sizeof(body));
    send_msg(g_client.sock_fd, &hdr, &body);
    pthread_setspecific(g_client.tls_key, NULL);
}

void taskhealth_heartbeat(void)
{
    if (!pthread_getspecific(g_client.tls_key))
        return;

    msg_body_heartbeat_t body = {
        .pid = g_client.pid,
        .tid = (int32_t)syscall(SYS_gettid),
    };

    taskhealth_msg_hdr_t hdr = build_hdr(MSG_HEARTBEAT, sizeof(body));
    ssize_t ret = send_msg(g_client.sock_fd, &hdr, &body);
    if (ret < 0 && errno == EPIPE) {
        /* daemon 崩溃或重启 → 标记未初始化，下次调用尝试重连 */
        close(g_client.sock_fd);
        g_client.sock_fd = -1;
        g_client.initialized = false;
    }
    /* 心跳消息单向，不等回复 */
}
```

### 4.4 客户端配置

```c
struct taskhealth_config {
    char    socket_path[256];  /* 守护进程 socket 路径，空 = 默认 /run/taskhealth.sock */
    int     _pad[16];          /* 预留，保持 ABI 兼容（原结构体字段不再使用） */
};
```

> **注意：** `check_interval_ms`、`max_threads`、回调函数指针已从 `taskhealth_config` 中移除，这些配置由守护进程（taskhealthd）管理。客户端配置只保留连接相关信息。

### 4.5 守护进程重连

```
heartbeat() 或 register() 检测到 EPIPE 时:
  1. close(sock_fd)
  2. g_client.initialized = false
  3. 下次 register() 调用时，自动走 init → connect 流程
  4. 调用方无感知，重新注册后即可正常使用
```

## 5. systemd 集成

```ini
# systemd/taskhealthd.service
[Unit]
Description=TaskHealth Thread Health Monitor Daemon
After=network.target

[Service]
Type=forking
ExecStart=/usr/sbin/taskhealthd
ExecReload=/bin/kill -HUP $MAINPID
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

## 6. 错误处理

| 场景 | 处理方式 |
|------|---------|
| 守护进程未启动 | client connect 失败 → `taskhealth_init` 返回 `TASKHEALTH_ERR_NOT_INIT` |
| 守护进程崩溃 | systemd 自动重启（Restart=always）；client send 返回 `EPIPE` → 标记 initialized=false，下次调用尝试重连 |
| client 进程崩溃 | epoll 检测到 hangup → 清理该 pid 所有条目，不触发 unexpected_exit |
| /proc 文件读取失败 | 静默跳过（返回 '?' / 空 wchan），下轮重试 |
| 注册表满 | daemon 回复 `MSG_STATUS_TABLE_FULL`，client 返回 `ERR_TABLE_FULL` |
| socket 缓冲区满 | `send` 阻塞等待（SEQPACKET 同步 socket，无 EAGAIN） |
| 心跳发送时 socket 断开 | `send` 返回 `EPIPE`，heartbeat() 静默返回（client 端仅记录，不重试） |
| daemon epoll 错误 | 除 EPOLLIN/HUP/ERR 外的事件忽略，保持循环运行 |

## 7. 配套互斥锁

`taskhealth_mutex_t` 仍在业务进程内使用，提供带锁名的 `pthread_mutex` 封装。

**锁名解析方式变化：**

| | 旧（进程内库） | 新（守护进程） |
|------|---------|---------|
| 解析方 | `taskhealth_mutex_resolve()` 调用 `dladdr` | 守护进程读 `/proc/<pid>/maps` |
| 地址比对 | 同进程，可直接比对虚地址 | 跨进程，用模块名+偏移替代地址比对 |
| 使用方式 | 线程卡住时库侧自动解析 | 告警中附 `module+0xoffset`，用户自行定位锁变量 |

> `taskhealth_mutex_resolve()` 函数废除，不再需要。守护进程通过 `/proc/<pid>/maps` 将 futex 地址解析为 "`/usr/lib/libxxx.so+0x12345`"，用户在源码中通过偏移定位具体锁变量。

---

Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
上海先道智觉科技有限责任公司 | Author: Lu Haoran 芦浩然
SPDX-License-Identifier: GPL-2.0-or-later
