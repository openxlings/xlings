# openkal：来自 Luban 的上游提案（草稿）

日期：2026-10-15　状态：已作为一个聚合 issue 提交（mcpplibs/openkal#47），只记录需求，不期望 openkal 现在改动

依据：Luban OS part 4 §6.5（`2026-10-15-luban-os-part4-design.md`）。每一项写清 Luban 需要什么、openkal 0.15
今天有什么、建议的接口，以及 Luban 怎样采用。`modules/platform` 里的实现按这里的形状写，提案被接受之后只换实现。

---

## 1. 伪终端（`openkal.pty`）

**需要。**

- Luban OS 应用的终端：创建一对伪终端，设置大小，以它为控制终端启动 shell，读写另一端，窗口变化时调整大小。
- 机器的 guest agent（`luban.vm`）对每个会话做同样的事。
- 三个平台需要同一份代码；Windows 上是 ConPTY。

**今天。** openkal 0.15 有终端的模式与大小，没有创建伪终端、也没有在伪终端上启动程序的办法。
Luban 在 `modules/platform` 里分别实现（POSIX `posix_openpt`；Windows 未实现）。

**建议。**

```c
struct kal_pty;
int  kal_pty_open(int cols, int rows, struct kal_pty *out);
int  kal_pty_resize(struct kal_pty, int cols, int rows);
struct kal_stream kal_pty_stream(struct kal_pty);
void kal_pty_close(struct kal_pty);
/* kal_spawn 增加 terminal：子进程以它为控制终端与标准流，在自己的会话里启动 */
```

## 2. 本地 endpoint

**需要。** 机器的 supervisor 与控制面（openas 信封）的本地套接字：只有本用户能连接。
POSIX 上是 `AF_UNIX`（文件与目录只有所有者可进）；Windows 上是 `AF_UNIX`，或只授予所有者的命名管道。

**今天。** `kal_endpoint` 只有地址加端口（IPv4、IPv6）。

**建议。**

```c
int kal_endpoint_local(const char *path, struct kal_endpoint *out);
/* kal_listen 在 local endpoint 上创建只有调用者能连接的端点；
   kal_connect / kal_accept / 流操作与 TCP 相同；路径上的旧端点被替换，活着的报告为占用 */
```

## 3. 以一组显式能力启动进程；进程组的上限与整组终止

**需要。**

- 无人值守的子进程（xlings 的 `run_shell_unattended`，part 3）：在自己的会话里启动，没有控制终端，
  输入是空的，父进程退出时一并结束，有时限。
- 以后的 agent 沙箱：只把指定的通道交给子进程，子进程继承同样的限制。

**今天。** `kal_process_spawn`、`kal_process_channel`、`kal_process_terminate` 是起点。
它们还没有：新会话、无控制终端、"父进程退出时结束"、对整组的上限与终止。

**建议。**

- `kal_spawn` 的选项增加 `session`（新会话，无控制终端）和 `bound`（父进程退出时结束）；
- 进程组：`kal_group_open(limits)`、`kal_spawn` 的 `group`、`kal_group_terminate`；
  限额包括 CPU、内存、进程数，各平台能做到的部分由查询回答，做不到的不声明。

## 4. 以根为界的文件视图；只读绑定

**需要。** agent 沙箱的文件通道：一个进程只看到一个根下的文件，额外的路径以只读或可写的形式授予。

**今天。** `openkal.fs` 处处是相对路径，这是这种视图的起点；还没有"以一个根为界启动一个进程"，也没有绑定。

**建议。** 先讨论：这一项是否属于 openkal，还是属于启动程序的那一方（Luban）以 openkal 的进程与文件接口组合出来。
openkal §11.6 把"谁能读"定为环境的责任；按这条原则，Luban 倾向于后者，这里只请 openkal 确认 `kal_spawn` 能接受
一组预先打开的目录作为子进程的全部文件视图。

---

## 采用顺序（Luban）

1. pty：Luban OS 应用在 Windows 上的终端，与 guest agent 共用；
2. 本地 endpoint：控制面；
3. 进程：无人值守子进程；
4. 文件视图：agent 沙箱（part 5）。
