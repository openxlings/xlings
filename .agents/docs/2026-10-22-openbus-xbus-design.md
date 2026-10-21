# Luban OS part 2：openbus 规范与它的实现 XBus——通用分布式软总线

日期：2026-10-22　状态：初稿，与 part 1（`2026-10-21-luban-os-ecosystem-usability-design.md`）一起实施，同一个 PR　缘起：part 1 实施中，
Luban OS 里已经出现了七套各自实现的通信（§1）；同时设备、跨设备、跨网络、GPU 内存、推理服务都需要同一种"谁能和谁说话、怎么说"的模型。

范围：
- **规范**：`luban/specs/openbus`——模型、线格式、交互方式、发现、路由、安全、可观察性的约定，以及一致性测试。只依赖标准库。
- **实现**：`luban/kits/xbus`——路由器、节点库、跨跳的链路、命令行 `xbus`（含监视、统计、基准）。只依赖标准库、规范与它自己的系统绑定（`src/native/`）。
- **接入**（本轮）：machine 的 agent 与宿主之间的链路；xdesk 的状态与事件；luban 的系统生命周期事件；openhal 的设备发现；
  Luban OS 应用与 `luban status` 的观察入口。
- **不在本轮**：网络传输（QUIC/TLS）、加密、GPU 内存区域的实际驱动、分布式调度、推理服务；它们在模型里有位置（§4、§13），本轮不实现。

## 0. 核心点

1. **一个模型**：总线上只有"节点"和"端点"。设备、应用、服务、虚拟设备、话题都是端点，用层级路径命名
   （`host/<id>/system/desk/device/display/0`、`…/service/xdesk/state`）。
2. **六种交互方式，一套帧**：调用（请求/响应，可流式回应、可取消）、发布/订阅（可保留最新值）、广播、流（有序、有流控）、
   事件（极轻的信号）、区域（共享内存，零拷贝）。帧头固定，载荷有"编码 id"：JSON、原始字节、自定义编码都行。
3. **控制面经路由，数据面直连**：每台主机（以及每个 Luban 系统）一个轻量路由，负责注册、发现、授权与统计；授权通过之后，
   同一主机上的两个节点直接拿到一对套接字通信，大数据走共享区域——吸取 bus1、dbus-broker 的教训：路由不做数据的中转站。
4. **跨跳就是路由之间的链路**：machine 的 virtio-serial、vz 的 vsock、WSL2 的 hvsocket、以后的网络，都是"两个路由之间的一条字节流"。
   part 1 的 carrier `Transport`（K3）就是 openbus 的传输绑定，不再另起一层。
5. **设备经 openhal**：openhal 负责向下发现与连接设备，路由把它们作为 `device/…` 端点放上总线；虚拟设备（openhal-emu）同样如此。
6. **身份与授权沿用已有规范**：每个连接有 openas 的身份（本机用对端凭据，跨跳用链路的身份），端点按策略授权（与 broker 同一个 judge 的思路），
   能力名进 opencaps。
7. **可观察性是规范的一部分**：每条消息带追踪 id；每个路由都必须提供 `bus/stats`、`bus/trace`、`bus/topology` 三个端点；
   `xbus monitor`、`xbus top`、`xbus bench` 让通信可以被看见、被测量；结果进 L5 的性能预算。
8. **兼容**：版本与特性在握手里协商；现有的七套通道本轮保留，逐个迁移（§11）。

## 1. 现状：七套通信

| 通道 | 方式 | 问题 |
|---|---|---|
| machine agent 的帧（H/O/D/X/B/A…） | virtio-serial 上的多路复用 | 每加一种用途加一种帧类型；旧端看不懂只能丢弃（part 1 的 broker 为此加了 A 帧） |
| opensal wire（帧、输入、剪贴板） | 带类型的流 | 帧挤在同一条流里（J11） |
| broker | SEQPACKET + 传 fd | 只有沙箱有，machine 另写了一遍 |
| xdesk 控制 socket、`subscribe` | JSON 行 | 只在 xdesk 自己的运行目录里，宿主看不到 |
| xlings `interface` 与 luban 的 Port | 子进程 + NDJSON | 只能一问一答，不能订阅 |
| 声音的 guestfwd | 一个 TCP 端口 | 旁路，看不到也测不到 |
| 两跳转发（宿主机里的 luban） | 手写转发 | 每种用途写一遍 |

共同的问题：没有统一的命名与发现；没有统一的授权；看不到谁在和谁说话、说了多少、慢在哪里；每多一个平台、一种设备，就多一套。

## 2. 调研与对比（2026-10）

| 系统 | 模型 | 取 | 不取 |
|---|---|---|---|
| D-Bus / dbus-broker | 总线守护进程转发所有消息；对象路径 + 接口 | 路径命名、内省；dbus-broker 的"总线不做策略之外的事、用最小的控制通道" | 所有数据经守护进程（多一次上下文切换和拷贝） |
| kdbus / bus1 | 内核里的总线；bus1 是能力模型 | 授权之后对等直连；memfd 传大块数据 | 依赖内核改动 |
| varlink（systemd 正在转向） | 点对点 JSON，unix socket | 简单、可用 `nc` 调试、有 IDL；用 pidfd 认人 | 没有发布订阅、没有发现 |
| Android Binder | 内核驱动，对象引用 + fd，Service Manager | 名字注册表（id 0 的根对象）；同步调用与 oneway | 绑在内核驱动上 |
| Fuchsia Zircon + FIDL | channel 传数据和 handle，handle 带权限且"移交"而不是复制 | 能力随消息移交；类型化的 handle | 只为本机，不跨机器（官方明说） |
| OpenHarmony DSoftBus | 发现、组网、连接、传输四层；BLE/CoAP 发现，按 QoS 选通道（lane），认证 + 加密 | 发现与传输分层；按 QoS 选链路；设备上线下线的心跳 | 绑在它自己的设备与系统框架上 |
| DDS / ROS 2 | 以数据为中心的发布订阅，QoS 丰富 | QoS 分类（可靠、历史、期限） | 配置复杂，小消息延迟高（ROS 2 实测 µs 级 vs iceoryx2 的百纳秒级） |
| Zenoh | 命名数据（key 表达式）上的发布订阅 + 查询；路由与对等两种拓扑；SHM；5 字节最小开销 | key 表达式与通配；路由/对等混合拓扑；本地消息不过传输层；共享内存 | 存储与计算插件不在本轮 |
| iceoryx2 | 共享内存零拷贝：发布订阅、请求响应、事件 | 大载荷走共享内存、单向约 150 ns；事件作为独立的轻量原语 | 轮询间隔带来的端到端延迟（需要事件唤醒） |
| MQTT / NATS | 经 broker 的话题 | 话题通配、保留消息 | 本机场景太重 |
| gRPC | HTTP/2 上的 RPC，流式 | 流式调用、取消、期限 | 本机延迟高、依赖重 |
| UCX / NIXL | 统一的 RDMA、CUDA IPC、共享内存、TCP；注册内存区域后一侧读写 | "注册区域 + 按两端都有的后端选传输"；必须能看见实际选了哪条（NIXL 静默退回 TCP 的教训） | 本轮不接 GPU |

**结论**：没有一个现成的实现同时满足"本机零拷贝 + 跨虚拟机/跨设备/跨网络 + 能力授权 + 可观察 + C++23 模块 + 一个静态二进制"。
因此我们只取**模型**：Zenoh 的命名与拓扑、iceoryx2 的共享区域与事件、bus1/Zircon 的"授权后直连、能力随消息移交"、
DSoftBus 的发现/组网/传输分层与 QoS 选路、varlink 的简单与可调试、UCX 的"注册区域、协商传输、看得见实际路径"。
规范自己写（openbus），实现自己写（XBus），与以上系统做桥接而不是绑定。

## 3. 原则

1. **命名的数据与服务**：一切都是路径上的端点；订阅与发现用通配。
2. **控制面与数据面分开**：路由管注册、发现、授权、统计；数据直连或走区域。
3. **能力式授权**：拿到一个通道就是拿到了权限；通道可以移交，不能伪造。
4. **传输无关**：同样的帧在 unix socket、virtio-serial、vsock、hvsocket、以后的网络上都一样；传输声明自己的能力（能否传 fd、能否共享区域）。
5. **看得见**：每条消息可追踪，每个路由可统计，实际走了哪条路一定能查到。
6. **小而稳**：路由单线程事件循环，不做数据之外的事；一个节点崩溃不影响别人；路由崩溃后节点自动重连并重新注册。

## 4. openbus 模型（规范 0.1）

### 4.1 节点与端点

- **节点**：一个参与者——进程、进程里的组件、guest agent、虚拟设备。有 128 位随机 id、可读的名字、openas 的身份（actor）。
- **端点**：节点在路径上提供的东西，种类：`service`（可调用）、`topic`（可订阅）、`device`（openhal 的设备，可调用 + 可订阅）、
  `stream`（可打开的流）、`region`（可映射的共享区域）。
- **路径**：`/` 分隔的段，字符集 `[a-z0-9._-]`；约定的根：
  - `host/<host-id>/…`：这台主机；
  - `host/<host-id>/system/<name>/…`：其中的 Luban 系统（沙箱或 machine）；
  - 段之后是 `service/…`、`topic/…`、`device/<class>/<n>`、`app/<app-id>/…`、`bus/…`（路由自己的）。
  - 通配：`*` 匹配一段，`**` 匹配任意多段（订阅与发现用）。
- **作用域**（发现与广播能到哪里）：`node` < `host` < `system` < `link`（跨跳）< `network`（以后）。

### 4.2 交互方式

| 方式 | 语义 | 用于 |
|---|---|---|
| call | 请求 → 一个或多个回应（流式），可取消，可设期限 | 服务调用、设备操作、`state` |
| publish/subscribe | 话题；可选"保留最新值"（新订阅者立刻收到） | 事件、设备状态、系统生命周期 |
| broadcast | 发给作用域里的所有节点，不保证送达 | 发现、通知 |
| stream | 两个端点之间有序、有流控的消息流（可双向） | 终端、帧、声音、日志 |
| event | 只带一个 64 位值的信号，可合并 | 唤醒、计数、"有新数据了" |
| region | 共享内存：宣告（名字、大小、格式）、映射、封口；消息里引用区域里的偏移 | 帧、大块数据；以后的 GPU 内存（dmabuf、CUDA IPC 句柄） |

### 4.3 帧（线格式）

固定 24 字节帧头（小端）：

```
magic "OB"(2) · version(1) · kind(1) · flags(2) · codec(2) · channel(4) · seq(4) · length(4) · trace(4: 追踪 id 的低 32 位；flags 表示后面另有 16 字节的完整追踪上下文)
```

- `kind`：hello、welcome、offer、withdraw、resolve、resolved、call、reply、reply-end、cancel、publish、subscribe、unsubscribe、
  broadcast、stream-open、stream-data、stream-close、credit（流控）、event、region-announce、region-attach、error、stats、ping、pong。
- `codec`：0 原始字节，1 JSON，2 openbus 紧凑键值，`≥ 0x100` 自定义（由端点声明）。
- 附带：本地传输上可以附带描述符（fd、handle），由传输的能力决定；不支持时退化为区域 id 或内联拷贝，并在统计里记一次"退化"。
- 上限：单帧载荷 1 MiB；更大的走 stream（分片）或 region。
- 未知的 `kind` 跳过并回一个 `error(unknown-kind)`；未知的 `codec` 不解码，原样转交（路由不需要理解载荷）。

### 4.4 握手与版本

`hello {openbus, node{id,name,actor}, features[], transports[]}` → `welcome {router, scope, features[], limits}`。
特性名：`regions`、`fds`、`direct`、`retained`、`trace-full`、`credit`……；双方只用交集。0.x 之内只增不改。

### 4.5 发现与存活

- 节点 `offer(path, kind, schema?, qos?)`；路由登记并向订阅了 `bus/topology` 或匹配通配的节点发布变化。
- 存活：连接即租约；连接断开，路由撤回它的全部端点并发布 `withdraw`。跨跳时，链路断开就撤回对面的全部端点。
- 设备：openhal 是"发现提供者"——路由（或它的一个插件节点）用 openhal 枚举设备，以 `device/<class>/<n>` 提供，设备变化即发布。

### 4.6 路由与直连

- 同一路由下：`call`、`publish` 默认经路由转发（小消息，延迟 µs 级）；节点可以对频繁的对端请求 `direct`，
  路由完成授权后发给双方一对已连接的套接字（本地传输支持传 fd 时），之后的帧不经路由。
- 跨跳：路由之间一条链路；对面的端点以前缀 `host/<id>/system/<name>/` 出现在本地拓扑里；消息按路径前缀转发。
- 路由不解码载荷，只读帧头；统计按端点、按方向累加。

### 4.7 安全

- 身份：本地连接用对端凭据（Linux 的 `SO_PEERCRED`/pidfd、macOS 的 `LOCAL_PEERCRED`）；链路由建立它的一方担保（machine 的链路就是那个系统）。
- 授权：每个端点有访问规则（谁可以 call/subscribe/offer 什么路径）；路由在 `resolve`、`subscribe`、`direct` 时判断，判断与记录用 openas 的格式。
- 能力：`direct` 得到的通道就是能力；可以经消息移交给另一个节点（本地传 fd），不能伪造。
- 跨网络（以后）：链路层加密与设备认证（DSoftBus、Zenoh 的做法）。

### 4.8 QoS

- 优先级：`control` > `interactive` > `bulk`；路由按优先级出队，保证控制消息不被大流量饿死。
- 可靠性：`reliable`（默认）/ `latest`（只保留最新一条，适合帧与状态）/ `best-effort`。
- 流控：stream 用 credit；publish 对慢订阅者按 QoS 丢弃或断开，并计数。

### 4.9 可观察性（规范要求）

每个路由必须提供：
- `bus/stats`（call）：每个端点、每个节点的消息数、字节数、错误、丢弃、延迟直方图（p50、p99、max），队列深度，传输与退化次数；
- `bus/trace`（subscribe）：采样或全部的帧头流（不含载荷，载荷按需打开），用于 `xbus monitor`；
- `bus/topology`（call + subscribe）：节点、端点、链路、实际使用的传输。

每条消息带追踪 id，跨跳不变；节点可以把同一个追踪 id 写进自己的日志（xlings 的 ops 日志、openas 的 journal）。

## 5. 与其它规范的分工

| 规范 | 与 openbus 的关系 |
|---|---|
| openkal | 内核资源；以后 openkal 提供套接字与共享内存时，XBus 的系统绑定改用它 |
| openhal | 向下发现与连接设备；设备以 `device/…` 端点出现在总线上 |
| opensal | 表面的消息；以后作为一个 stream 端点（`…/service/opensal/<display>`）跑在 openbus 上，帧用 region |
| openas | 身份与记录：连接的 actor、授权判断的记录格式 |
| opencaps | 能力名：`openbus`、`openbus-regions`、`openbus-direct` |
| carrier（part 1 K3 的 `Transport`） | openbus 的传输绑定：字节流 + 可选的共享区域 + 多跳 |

规范之间仍然互不引用；连接它们的是实现（XBus、xdesk、luban）。

## 6. XBus（实现）

### 6.1 包与模块

```
luban/specs/openbus/     SPEC.md、src/（openbus：路径与通配、帧编解码、握手、kind、统计的格式）、conformance/
luban/kits/xbus/         src/
                           native/   xbus.native：套接字、poll、传 fd、memfd/shm、对端凭据（唯一含系统头的地方）
                           router    xbus.router：路由（单线程事件循环）
                           node      xbus.node：节点库（C++ API）
                           link      xbus.link：路由之间的链路（任意双向字节流）
                           stats     xbus.stats：直方图与计数
                         tool/       xbus：命令行（router、ls、call、pub、sub、monitor、top、stats、bench）
```

依赖：规范只依赖标准库；kit 依赖标准库、openbus 规范与它自己的 native 模块；不引用 `xlings.*`、`luban.*`。

### 6.2 路由

- 一个进程、一个线程、一个 `poll` 循环；每个连接一个发送队列（按优先级）；读帧头、按路径表转发。
- 路径表：精确匹配用哈希，通配订阅用前缀树。
- 统计：每个端点的计数与直方图（对数分桶，固定内存）。
- 追踪：一个固定大小的环形缓冲，`bus/trace` 的订阅者从这里读；没有订阅者时不记录载荷之外的任何东西，开销接近零。
- 崩溃与重启：节点断线后指数退避重连，重连后重新 offer、重新 subscribe（节点库自动完成）。

### 6.3 节点库

```cpp
auto node = xbus::Node::connect({.name = "xdesk", .socket = xbus::default_socket()});
node->offer("service/xdesk/state", [](const xbus::Call& c) { return c.reply_json(state()); });
auto events = node->offer_topic("topic/xdesk/events", {.retained = false});
events.publish_json({{"event", "focus"}, {"id", 3}});
auto r = node->call("host/*/system/desk/service/xdesk/state", {}, {.timeout = 2s});
node->subscribe("host/**/topic/luban/systems", [](const xbus::Message& m) { ... });
```

- 同步与异步两种用法；回调在节点自己的线程上运行（或交给调用方的事件循环：`node->fd()` + `node->dispatch()`，xdesk 这样的 wayland 循环直接嵌入）。

### 6.4 链路与跨跳

- `xbus::Link` 把两个路由连在一条字节流上：本轮用于 machine 的 agent 通道（guest 的路由 ↔ 宿主的路由）。
- part 1 的 agent 协议加一种通道用途"openbus 链路"：supervisor 收到 guest 的 hello 后，为链路打开一个通道，两端各接到本地路由。
  旧的一方没有这个通道就不连，其它功能不受影响。

### 6.5 路由在哪里运行

- **宿主**：每个 xlings home 一个路由，socket 在 `<home>/run/openbus/router.sock`；由第一个需要它的节点自动拉起（`xbus router --detach`，
  单实例锁），空闲 10 分钟且无节点时退出。luban、luban-os、xdesk（沙箱里经绑定的 socket）都是它的节点。
- **Luban 系统（machine）**：guest agent 内置一个路由（agent 本来就常驻），socket 在 `/run/openbus/router.sock`，并通过链路连到宿主。
- **沙箱系统**：沙箱里的 socket 绑定到宿主路由；沙箱里的节点身份是那个系统。
- **真机**：系统自己的路由（由 init 启动），没有上游链路。

## 7. 本轮的接入

| 接入 | 端点 | 说明 |
|---|---|---|
| 路由自身 | `bus/stats`、`bus/trace`、`bus/topology` | 规范要求 |
| machine 链路 | 对面系统的全部端点 | 宿主上 `xbus ls` 能看到 machine 里的端点 |
| xdesk | `…/service/xdesk/state`（call）、`…/topic/xdesk/events`（publish）、`…/service/xdesk/hud`（call） | 与控制 socket 共用实现；控制 socket 保留 |
| luban 服务 | `host/<id>/topic/luban/systems` | 系统的创建、启动、停止、删除；保留最新值 |
| openhal | `host/<id>/device/<class>/<n>` | 宿主的设备（openhal-host 的枚举）；以后加热插拔 |
| 观察入口 | `xbus ls/top/monitor/stats/bench`；`luban status <名字> --bus`；Luban OS 应用的"关于"页显示路由与链路 | |

不迁移的：broker（它要传终端与 fd，下一轮改为 openbus 的 stream + 能力移交）、opensal 的帧（K7 之后再迁到 region）、声音。

## 8. 可观察性、剖析与调试

- **监视**：`xbus monitor [路径通配]` 实时打印帧头（时间、方向、kind、路径、大小、延迟、追踪 id），`--payload` 打开载荷（JSON 美化、二进制十六进制前 64 字节）。
- **统计**：`xbus top` 按端点排序的消息率、字节率、p99 延迟、丢弃；`xbus stats --json` 给脚本与 CI。
- **追踪**：`xbus trace <追踪 id>` 收集一条消息跨跳的每一段时间（路由收、发、链路、对面路由）。
- **日志**：路由的事件（连接、断开、授权拒绝、退化、慢消费者）写 `<home>/logs/openbus/router.ndjson`；默认输出只有结论，细节在 `-v`。
- **基准**：`xbus bench call|pubsub|stream|region|link`：报告 p50/p99/p999、每秒消息数、吞吐；`--json` 进 L5；
  CI 跑相对回归（与 part 1 §5.6a 同一规则：先只报告，稳定后退步超过 25% 算失败）。

## 9. 性能预算（本机，参考机器上本地验收；CI 比相对值）

| 场景 | 预算 |
|---|---|
| call 经路由，64 B，往返 | p50 ≤ 40 µs，p99 ≤ 150 µs |
| call 直连，64 B，往返 | p50 ≤ 20 µs |
| publish 1 KB，1 发 4 收 | ≥ 200 k 消息/秒，p99 ≤ 200 µs |
| region 宣告 1080p 帧（只发矩形） | ≤ 20 µs/帧，不拷贝像素 |
| 跨 machine 链路 call 往返 | 实测后定，写进报告 |
| 路由空闲 | 0 % CPU（无定时器空转） |

## 10. 跨平台

- **Linux**：全部功能（SOCK_STREAM + SCM_RIGHTS、memfd、SO_PEERCRED）。
- **macOS**：AF_UNIX SOCK_STREAM + SCM_RIGHTS、`shm_open`、`LOCAL_PEERCRED`；全部功能。
- **Windows**：AF_UNIX SOCK_STREAM（Windows 10 起）可用，不能传 fd：`direct` 与 `region` 退化（区域内联拷贝），本轮先保证 call、pub/sub、stream 可用；
  命名共享内存与句柄复制下一轮。
- 帧格式与平台无关；退化一律计数并在 `bus/topology` 里可见。

## 11. 兼容与迁移

- 现有通道全部保留；新端点与旧接口并存（xdesk 的控制 socket、agent 的帧）。
- 新宿主 + 旧系统：agent 没有 openbus 链路，宿主照旧工作。旧宿主 + 新系统：没有链路通道，guest 的路由只服务系统内部。
- 迁移顺序（以后）：broker → opensal 帧 → 声音 → luban 的 Port（订阅）→ 外部协议桥（D-Bus、MQTT、Zenoh）。

## 12. 测试

| 层 | 内容 |
|---|---|
| 规范一致性 | 路径与通配、帧编解码（含未知 kind、未知 codec、超限）、握手的特性交集 |
| 单元 | 路由表、直方图、流控、优先级队列、重连 |
| e2e | 一个路由 + 多个节点：call、流式回应、取消、pub/sub（保留值、慢订阅者）、broadcast、stream、event、region（memfd）、direct；
  两个路由经链路（管道模拟一跳）；断开时撤回；授权拒绝 |
| 接入 | xdesk 的 state 与 events 经总线；luban 的系统事件；machine 链路（本地 KVM，luban-os-ci 的 L2） |
| 基准 | `xbus bench` 的各项；CI 相对回归 |

## 13. 以后（模型里已有位置）

- 网络：QUIC/TLS 链路、mDNS/CoAP 发现、设备认证与端到端加密（DSoftBus、Zenoh 的做法）。
- GPU：region 的种类加 `dmabuf`、`cuda-ipc`、`rdma`；按两端都有的后端选择，必须能看见实际路径（UCX/NIXL 的教训）。
- 分布式调度与推理：作为服务跑在 openbus 上（call + stream 回 token，KV cache 用 region）。
- 外部协议桥：D-Bus（托盘的 SNI）、MQTT、Zenoh。

## 14. 检查点（并入 part 1 的 xlings PR）

| 检查点 | 内容 | 依赖 |
|---|---|---|
| B1 | openbus 规范：SPEC.md、`openbus` 模块（路径、通配、帧、握手）、一致性测试 | — |
| B2 | XBus native 与路由：连接、注册、call、pub/sub、broadcast、event、统计、追踪、拓扑 | B1 |
| B3 | 节点库与 `xbus` 命令行（router、ls、call、pub、sub、monitor、top、stats） | B2 |
| B4 | stream、credit、region、direct；`xbus bench`；性能预算进 L5 | B3 |
| B5 | 链路：两个路由经字节流；machine 的 agent 通道接入 | B3、part 1 K4 |
| B6 | 接入：xdesk 的 state/events/hud、luban 的系统事件、openhal 的设备、`luban status --bus`、Luban OS 应用的"关于" | B3、K12 |
| B7 | 文档（`docs/luban` 的 openbus 一节、规范 README）、需求编号 | 全部 |

part 1 的 K3（`Transport`）按 openbus 的传输绑定来定，K7 的帧以后迁到 region。

## 15. 自审

| 视角 | 发现 | 处理 |
|---|---|---|
| 架构 | 规范与实现分开，避免"规范就是某个实现的 API" | `openbus` 只定义模型与线格式；`xbus` 是第一个实现 |
| 架构 | 路由成为瓶颈 | 控制面经路由、数据面直连或 region；路由只读帧头 |
| 简洁 | 交互方式太多 | 六种都由同一套帧表达；event 与 region 是 iceoryx2 证明过必要的 |
| 稳定性 | 路由崩溃 | 节点自动重连并恢复注册；路由单线程、无载荷解析 |
| 性能 | JSON 慢 | 编码 id 让热路径用原始字节或紧凑格式；JSON 只用在控制面与调试 |
| 安全 | 路由被当成越权的跳板 | 授权在路由判断并记录；direct 是能力，不能伪造 |
| 跨平台 | Windows 不能传 fd | 退化并计数，功能先保证可用 |
| 兼容 | 一次迁移所有通道风险大 | 本轮只新增与接入观察类端点，旧通道保留 |
| 可观察 | "选了哪条路"看不见（NIXL 的教训） | `bus/topology` 记录实际传输与退化次数 |

## 16. 来源

- [OpenHarmony DSoftBus](https://github.com/openharmony/communication_dsoftbus)、[DSoftBus 代码分析](https://deepwiki.com/openharmony/communication_dsoftbus)
- [Zenoh](https://zenoh.io/)、[eclipse-zenoh/zenoh](https://github.com/eclipse-zenoh/zenoh)、[Zenoh 1.7.x](https://zenoh.io/blog/2025-12-11-zenoh-jiaolong/)
- [eclipse-iceoryx/iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2)、[iceoryx2 benchmarks](https://github.com/eclipse-iceoryx/iceoryx2/blob/main/benchmarks/README.md)、[iceoryx2 讨论 #435](https://github.com/eclipse-iceoryx/iceoryx2/discussions/435)
- [Rethinking the D-Bus Message Bus](https://dvdhrm.github.io/rethinking-the-dbus-message-bus/)、[Bus1 (LWN)](https://lwn.net/Articles/697191/)
- [Zircon 基础](https://fuchsia.dev/fuchsia-src/get-started/learn/intro/zircon)、[FIDL](https://fuchsia.dev/fuchsia-src/get-started/learn/fidl)
- [sd-varlink](https://man7.org/linux/man-pages/man3/sd-varlink.3.html)、[systemd 与 varlink](https://www.phoronix.com/news/Systemd-Varlink-D-Bus-Future)
- [NIXL 与解耦推理](https://www.spheron.network/blog/nvidia-nixl-disaggregated-inference-guide/)、[vLLM NixlConnector](https://docs.vllm.ai/en/stable/features/nixl_connector_usage/)、[vLLM #62080（静默退回 TCP）](https://github.com/vllm-project/vllm/issues/62080)
