# 09 —— 总线：系统与谁说话

[English](../../luban/09-bus.md) | **简体中文**

每个 Luban 系统都有一条总线，宿主也有：程序用同一种方式把自己能做的暴露出来、使用别人暴露的——桌面的状态、一个设备、
一个服务——不论它在这个系统里、在宿主上，还是在另一台 machine 里。总线遵循 **openbus**（`luban/specs/openbus/SPEC.md`），
**XBus** 是它的实现。

## 总线在哪里

| 位置 | 它的路由 | 上面有什么 |
|---|---|---|
| 宿主 | 每个 xlings home 一个，在 `<home>/run/openbus/router.sock`；第一个需要它的一方拉起（machine 启动时），空闲十分钟后退出 | 链接在它下面的系统，以及宿主上的程序 |
| machine | 在它的 agent 里，`/run/openbus/router.sock`；它的程序经 `OPENBUS_ROUTER` 找到 | 桌面（`service/xdesk/state`、`service/xdesk/control`、`topic/xdesk/events`），以及它的程序暴露的其它对象 |

machine 运行时，它的总线链接在宿主的下面：在宿主上，它的对象位于 `host/<宿主>/system/<名字>/...`，用起来和本地的一样。
总线上的一切都能用同一种方式观察：每个程序回答自己的 telemetry（计数、延迟直方图、最近的日志、剖析、转储）。

## 查看

```bash
luban status --bus                 # the host's bus: the systems linked under it
luban status box --bus             # what box's programs offer, seen from the host
luban status box --bus --json
```

`xbus`（与 `luban` 放在一起）直接使用它：

```bash
export OPENBUS_ROUTER=unix:$HOME/.xlings/run/openbus/router.sock
xbus ls                                            # every object, with its interfaces
xbus ls --interface openbus.telemetry@0.1          # every program that can be asked
xbus call host/<host>/system/box/service/xdesk/state
xbus dump host/<host>/system/box/node/<id>/telemetry          # its metrics, state, recent log
xbus profile host/<host>/system/box/node/<id>/telemetry --seconds 5   # a trace (open it in Perfetto)
xbus stats | xbus top | xbus monitor               # what the router carries
xbus bench call|stream|region                      # how fast, here
```

路由自己的日志是 `<home>/logs/openbus/router.ndjson`（一行一个 JSON 对象）；machine 里的是它的 `/run/openbus/router.ndjson`。

## 还不是什么

跨网络的链路、两个程序之间的直连、以及把显示的帧做成共享区域，是下一步（设计 2026-10-22 §13、§14d）。旧版 luban 启动的
machine、或旧系统的 agent 没有总线：其它一切照旧。
