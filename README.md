# NeuroCast

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.20+-blue.svg)](https://cmake.org/)
[![C++14](https://img.shields.io/badge/C++-14-blue.svg)](https://isocpp.org/)

嵌入式 IP 摄像头固件框架。以 WebRTC 实时推流为核心，覆盖媒体服务、云端对接、触发自动化、OTA 升级的完整能力，通过平台抽象层实现一套代码适配不同芯片。

## 核心能力

- **WebRTC 实时推流** — 支持 P2P（浏览器直连，MQTT 信令，Full-ICE）和 SFU（WHIP 推到 SRS，WHEP 拉流，多人观看）两种模式。浏览器零插件直接看，亚秒延迟
- **媒体服务** — MP4 录像（可配分段时长）、JPEG 拍照（手动 + 触发自动拍）、OSD 水印（时间/文字/图形/位图，跨分辨率自适应）
- **触发源自动化** — 可插拔的触发源架构（定时器、蓝牙、录像联动），优先级排队调度
- **云端对接** — iot_agent 进程统一处理 MQTT 连接、配置同步、文件上传、RPC 命令转发，对接 ThingsBoard 等平台
- **OTA 升级** — 固件下载 → 校验 → 烧写全流程，支持硬件分区级升级
- **平台抽象层（PAL）** — 纯接口定义，硬件相关代码收敛在 backends 目录，应用层和库层零平台依赖

## 平台支持

- **anyka-av100**（Anychip AK39AV100）— 已完整适配，摄像头/录像/编码器/OSD/推流全部跑通，已交付日本客户正式商用
- **linux-generic** — 通用 Linux 后端，软件渲染 OSD、文件模拟固件烧写，用于 x86 宿主机开发调试
- **mock** — 单元测试桩，所有接口返回模拟数据

框架设计上，新增一个硬件平台只需要加一个 backend 目录和一个 CMake preset，应用层代码零改动。

## 背景与由来

这套框架源自一个实际问题：传统嵌入式摄像头固件都是在厂商 PDK 源码树里原地开发，业务代码和硬件调用搅在一起，换芯片等于重写，代码改不动也测不了。每接一个云平台还要重新写一套 MQTT + 配置 + OTA，推流方案停留在 RTSP 或私有协议，浏览器看不了。

NeuroCast 的做法是把应用逻辑和硬件平台彻底分开：上层是纯业务代码，中间是通用库，底层是平台抽象接口。厂商 SDK 只在 backends 目录里出现，x86 上就能跑单元测试。推流选择 WebRTC 作为核心方向 — 浏览器原生支持、亚秒延迟、NAT 穿透、多人观看，这些是 RTSP 做不到的。架构预留了扩展空间，未来可以接入其他推流协议。

## 架构

```
┌─────────────────────────────────────────────────────────┐
│  apps/                                                  │
│    mediad/        Media daemon (camera, recording,      │
│                   streaming, OSD, triggers)             │
│    iot_agent/     Cloud agent (MQTT, config sync, OTA)  │
├─────────────────────────────────────────────────────────┤
│  libs/                                                  │
│    camera/        Camera driver abstraction             │
│    recorder/      MP4 recorder abstraction              │
│    osd/           OSD rendering engine                  │
│    common/        Shared utilities                      │
│    http/          HTTP client & download                │
│    mqtt/          MQTT client wrapper                   │
│    mq/            IPC message queue (ZeroMQ)            │
│    config_sync/   Config synchronization protocol       │
│    sensor/        Sensor drivers (Bluetooth, Zigbee)    │
├─────────────────────────────────────────────────────────┤
│  pal/           Platform Abstraction Layer              │
│    include/pal/   Pure interfaces (zero vendor deps)    │
│    backends/      Platform implementations              │
│      linux-generic/  Software rendering backend         │
│      mock/           Test stubs                         │
└─────────────────────────────────────────────────────────┘
```

设计原则：

- **严格分层** — `apps → libs → pal 接口 ← backends`，厂商 SDK 头文件不允许出现在 `pal/backends/` 之外
- **平台抽象** — 新增硬件平台 = 新增一个 backend 目录 + 一个 toolchain 文件 + 一个 CMake preset，应用层和库层零改动
- **构建时选择平台** — `NC_PLATFORM` CMake 变量在编译时选定唯一后端，无运行时开销
- **配置热更新** — 所有服务支持运行时更新配置，无需重启（触发源、OSD、录像参数等）

## 快速开始

### 编译（x86 宿主机，含单元测试）

```bash
cmake --preset x86-debug
cmake --build --preset x86-debug
ctest --preset x86-debug
```

### 目录结构

- `pal/` — 平台抽象层（纯接口 + 各平台后端实现）
- `libs/` — 平台无关库（camera, recorder, OSD, HTTP, MQTT 等）
- `apps/` — 应用（mediad 媒体守护进程, iot_agent 云端代理）
- `tests/` — 单元测试（x86 + mock 后端）
- `cmake/` — CMake 模块和工具链文件
- `third_party/` — FetchContent 依赖声明
- `scripts/` — 构建和分析脚本

### 新增平台

1. 新建 `pal/backends/<platform>/`，实现 `pal/include/pal/*.h` 接口
2. 新建 `libs/camera/src/<platform>/`，实现 `CameraDriver`
3. 新建 `libs/recorder/src/<platform>/`，实现 `recorder_driver` / `demuxer_driver`
4. 在 `pal/CMakeLists.txt`、`libs/camera/CMakeLists.txt`、`libs/recorder/CMakeLists.txt` 加 `elseif` 分支
5. 在 `CMakePresets.json` 加一个构建预设

应用层和库层代码零改动。

## 开源版 vs 商用版

- ✅ 框架全部源码（开源版 / 商用版都有）
- ✅ 架构文档 & API 文档
- ✅ 单元测试套件
- ✅ x86 宿主机开发调试
- ❌ 硬件平台后端（芯片驱动适配）— 仅商用版
- ❌ 厂商 SDK 集成（摄像头/录像/编码器驱动）— 仅商用版
- ❌ 设备端部署配置 & 工具链 — 仅商用版
- ❌ 预编译固件 & 烧写工具 — 仅商用版
- ❌ 技术支持 & 定制开发 — 仅商用版

开源版可以查看架构、理解设计、跑通单元测试。要在真实硬件上运行，需要商用版。

👉 [了解商用版](COMMERCIAL.md)

## 文档

完整文档在仓库根目录的 [docs/](../docs/) 下。

- **[架构文档](../docs/firmware/architecture.md)** — 系统整体架构、IPC 拓扑、平台抽象策略、数据流
- **[WebRTC 推流](../docs/firmware/guides/webrtc-streaming.md)** — P2P/SFU 模式、MQTT 信令、WHIP/WHEP 协议
- [信令协议](../docs/firmware/api/push-streaming-protocol.md) — P2P/SFU 信令详细协议
- [IPC 协议](../docs/firmware/api/ipc-protocol.md) — 进程间通信协议
- [mediad 接口](../docs/firmware/api/mediad-api.md) — 媒体守护进程命令和事件参考
- [iot_agent 接口](../docs/firmware/api/iot-agent-api.md) — 云端代理 MQTT 接口
- [OSD 水印配置](../docs/firmware/api/osd-elements-config-api.md) — OSD 元素配置 API
- [触发源配置](../docs/firmware/api/triggers-config-api.md) — 触发自动化配置
- [配置参考](../docs/firmware/guides/config-reference.md) — 全部配置项说明

## 第三方依赖

构建时依赖（静态链接到固件二进制中）：

- [libzmq](https://github.com/zeromq/libzmq) 4.3.5 (LGPL-3.0) — IPC 消息队列
- [cppzmq](https://github.com/cppzmq/cppzmq) 4.11.0 (MIT) — ZeroMQ C++ 头文件绑定
- [cJSON](https://github.com/DaveGamble/cJSON) 1.7.19 (MIT) — JSON 解析
- [spdlog](https://github.com/gabime/spdlog) 1.17.0 (MIT) — 日志
- [OpenSSL](https://www.openssl.org/) (Apache 2.0) — TLS/SSL 和密码学
- [libcurl](https://curl.se/libcurl/) (MIT/X) — HTTP 客户端和文件下载
- [Eclipse Paho MQTT](https://www.eclipse.org/paho/) (EPL-1.0 / EDL-1.0) — MQTT 客户端
- [metaRTC](https://github.com/metartc/metaRTC) 8.0 (MIT) — WebRTC 引擎（P2P + WHIP）

测试依赖：

- [GoogleTest](https://github.com/google/googletest) (BSD-3-Clause) — 单元测试框架
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) (LGPL-2.1+) — 测试用假 HTTP 服务器

兼容服务器：

- [SRS](https://github.com/ossrs/srs) (MIT) — SFU 推流场景下的 WebRTC 中继服务器。设备通过 WHIP 推流到 SRS，观看端通过 WHEP 拉流，实现多人观看和 NAT 穿透

> libzmq (LGPL-3.0) 和 libmicrohttpd (LGPL-2.1+) 通过静态链接使用。根据 LGPL 条款，你可以用自行修改的版本重新链接。

## License

Apache License 2.0. See [LICENSE](LICENSE).

如有问题或合作意向，请发送邮件至 hnngm163@gmail.com。
