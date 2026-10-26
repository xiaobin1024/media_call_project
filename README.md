# P2P 音视频通话系统（Qt / C++17）

一个自研信令与媒体传输的音视频通话系统：Qt 桌面客户端 + C++ 信令/中继服务端，
支持登录、用户列表、呼叫接听、NAT 穿透、P2P 优先、失败回退 UDP 中继，以及中继后重新尝试 P2P 回切。

## 目录结构

```
callserver/   信令与中继服务端（C++17，select 单线程 Reactor，跨平台）
  src/socket_util.h        Winsock / POSIX 的跨平台 socket 封装
  src/signaling_server.*   Reactor 事件循环、TCP 信令路由、UDP 中继、路径仲裁状态机
  src/session.*            每连接会话：读缓冲分帧、写队列、信令消息处理
  test/                    Python 假客户端路径测试脚本
callclient/   Qt 6 音视频客户端
  src/signaling/           TCP 信令客户端
  src/call/                CallManager 通话状态机与媒体生命周期
  src/media/               摄像头采集（QThread）、音频采集/播放、帧渲染
  src/transport/           RTP 组帧/分片、JitterBuffer
  src/ui/                  登录窗口、通话窗口
docs/联调日志/              公网联调的原始客户端/服务端日志
```

## 构建

```bash
# 服务端（Linux / Windows 均可）
cd callserver && cmake -S . -B build && cmake --build build

# 客户端（需要 Qt 6 Widgets / Multimedia）
cd callclient && cmake -S . -B build && cmake --build build
```

## 运行

```bash
# 服务端：TCP 信令 8889，UDP 探测/中继 8890（= TCP 端口 + 1）
./call_server 0.0.0.0 8889
# 或 --port=8889
```

客户端在 `config.json` 中填写 `serverIp` / `serverPort` 后启动即可。

## 协议要点

**TCP 信令**：换行分隔的 JSON（每条以 `\n` 结尾）。消息类型：

| 类型 | 方向 | 说明 |
|---|---|---|
| `login` / `login_resp` | C→S / S→C | 登录与应答 |
| `user_list` | S→C | 在线用户广播 |
| `call_offer` / `call_answer` / `call_early` | C→S→C | 呼叫、接听、被叫提前回送公网端口（双向打洞） |
| `hang_up` | C→S→C | 挂断并释放中继绑定 |
| `port_discovered` | S→C | 服务端观察到的客户端公网 UDP 端口 |
| `relay_info` | S→C | 分配本次通话的中继 ID |
| `path_report` | C→S | 客户端上报是否收到直连包 |
| `path_decision` | S→C | 服务端下发的路径决策（含 mode 与 epoch） |

**SDP 格式**：`"publicPort:localPort"`。服务端按两端是否同网改写地址——
同网改用对方内网 IP + 本地端口；跨网且公网端口有效则用公网 IP + 公网端口，
否则退化为公网 IP + 本地端口，保证至少有一条可用路径。

**UDP**：

- 端口探测：客户端发 1 字节探针，服务端回 `port_discovered` 告知观察到的公网端口。
- 媒体中继：8 字节中继头 `magic(2) | version(1) | reserved(1) | relayId(4, 大端)`
  + RTP 负载。服务端通过首个数据报学习通话双方端点（源端口学习），随后双向转发。

## 路径仲裁状态机

```
Probing ──双方均收到直连──> P2pProbation ──观察 3s 稳定──> P2p
   │                             │
   │ 4s 超时或任一方未收到            │ 上报超时 / 任一方收不到
   └──────────────> Relay <────────┘
                      │
                      │ relay 后 10s 冷却，双方持续直连 3s 稳定
                      └──> P2pProbation（回切）
```

每次路径变更递增 `epoch`，用于隔离迟到的旧上报，避免状态抖动。

## 已知边界

- 服务端为**单线程 select** 事件循环，面向轻量信令场景，不是高并发模型。
- 视频为 MJPEG/JPEG over RTP 分片，**不是 H.264**；音频为 16bit PCM，复用 PCMA 的 PT=8。
- 状态全部在单机内存中，无持久化、无鉴权。
- 已完成**同网双端 + 公网服务端**联调；跨 NAT 的点对点直连与对称 NAT 场景仍在补测。
