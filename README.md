# NeuroCast

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.20+-blue.svg)](https://cmake.org/)
[![C++14](https://img.shields.io/badge/C++-14-blue.svg)](https://isocpp.org/)

**嵌入式 IP 摄像头固件框架，以 WebRTC 实时推流为核心。**

浏览器直接看、零插件零安装、亚秒延迟 — 这是 WebRTC 带来的体验变革，而目前几乎没有嵌入式摄像头固件原生支持它。NeuroCast 把这件事做好了。

## 解决什么问题？

- **厂商 PDK 绑架** — 固件写在厂商 SDK 源码树里，换芯片 = 重写全部。NeuroCast 用 PAL 平台抽象层解决，换硬件只加一个 backend 目录，应用层零改动
- **推流方案老旧** — 大部分摄像头用 RTSP 或私有协议，浏览器看不了，要装插件/App。内置 WebRTC（P2P + SFU），浏览器原生支持，零安装
- **代码像面团** — 业务逻辑和硬件调用搅在一起，改不动、测不了。严格分层 apps → libs → pal，x86 上就能跑单元测试
- **云端对接重复造轮子** — 每接一个云平台就写一套 MQTT + 配置 + OTA。iot_agent 统一对接，配置同步、OTA、文件上传开箱即用
- **仓库臃肿** — 厂商代码 + 开源库全塞 git，clone 一次等半天。业务仓库干净，依赖用 FetchContent 按版本拉取

## 为什么是 WebRTC？

- **浏览器支持** — RTSP 需要插件或转码服务器，WebRTC 原生支持，零安装
- **延迟** — RTSP 1-5 秒（转码后更高），WebRTC 亚秒级（P2P 直连最低）
- **移动端** — RTSP 需要专用 App，WebRTC H5 页面直接看
- **NAT 穿透** — RTSP 基本没有，WebRTC Full-ICE + STUN/TURN
- **多人观看** — RTSP 需要流媒体服务器转码，WebRTC SFU 模式（WHIP/WHEP）原生支持

越来越多的场景需要浏览器直接看摄像头（智能家居、门店监控、工地巡检、农业监控），不想装 App、不想部署转码服务器。WebRTC 是唯一能同时做到「浏览器直接看 + 亚秒延迟 + 多人观看」的方案。

NeuroCast 以 WebRTC 为核心，架构预留扩展 — 未来可以接入其他推流协议，但 WebRTC 是当前主打和核心差异化。

## Key Features

### WebRTC Real-Time Streaming

Built-in WebRTC support with two streaming modes:

- **P2P** — Direct browser-to-device connection via MQTT signaling, Full-ICE (host + STUN/TURN), lowest latency
- **SFU** — WHIP push to SRS server, WHEP pull by viewers, supports multiple viewers and NAT traversal fallback

See [WebRTC Streaming Guide](../docs/firmware/guides/webrtc-streaming.md) for the full protocol specification.

### Media Services

- **Recording** — MP4 recording with configurable segment duration
- **Snapshots** — JPEG capture with trigger-based automation (timer, bluetooth, SOS)
- **OSD Watermarks** — Cross-resolution overlay engine (time, text, shapes, bitmaps)
- **Trigger System** — Pluggable trigger sources with priority-based scheduling

## Architecture

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

### Key Design Principles

- **Strict layering**: `apps → libs → pal interfaces ← backends`. Vendor SDK headers never leak above `pal/backends/`.
- **Platform abstraction**: New hardware platform = new `pal/backends/<platform>/` directory + `libs/camera/src/<platform>/` + `libs/recorder/src/<platform>/`. Zero changes to apps or libs.
- **Build-time platform selection**: `NC_PLATFORM` CMake variable selects exactly one backend. No runtime overhead.
- **Hot-reloadable config**: All services support config updates without restart (triggers, OSD, recording parameters).

## Quick Start

### Build (x86 host, with unit tests)

```bash
cmake --preset x86-debug
cmake --build --preset x86-debug
ctest --preset x86-debug
```

### Project Structure

- `pal/` — Platform Abstraction Layer（纯接口 + 各平台后端）
- `libs/` — 平台无关库（camera, recorder, OSD, HTTP, MQTT 等）
- `apps/` — 应用（mediad, iot_agent）
- `tests/` — 单元测试（x86 + mock 后端）
- `cmake/` — CMake 模块和工具链文件
- `third_party/` — FetchContent 依赖声明
- `scripts/` — 构建和分析脚本

## Adding a New Platform

1. Create `pal/backends/<your-platform>/` implementing `pal/include/pal/*.h`
2. Create `libs/camera/src/<your-platform>/` implementing `CameraDriver`
3. Create `libs/recorder/src/<your-platform>/` implementing `recorder_driver` / `demuxer_driver`
4. Add `elseif(NC_PLATFORM STREQUAL "<your-platform>")` branches in `pal/CMakeLists.txt`, `libs/camera/CMakeLists.txt`, `libs/recorder/CMakeLists.txt`
5. Add a CMake preset in `CMakePresets.json`

The apps and libs layers require zero changes.

## Open Source vs Commercial

- ✅ 框架全部源码（开源版 / 商用版都有）
- ✅ 架构文档 & API 文档
- ✅ 单元测试套件
- ✅ x86 宿主机开发调试
- ❌ 硬件平台后端（芯片驱动适配）— 仅商用版
- ❌ 厂商 SDK 集成（摄像头/录像/编码器驱动）— 仅商用版
- ❌ 设备端部署配置 & 工具链 — 仅商用版
- ❌ 预编译固件 & 烧写工具 — 仅商用版
- ❌ 技术支持 & 定制开发 — 仅商用版

开源版让你看到架构、理解设计、跑通单元测试。**要在真实硬件上运行，需要商用版。**

👉 [了解商用版](COMMERCIAL.md)

## Documentation

Full documentation is in the [docs/](../docs/) directory at the repository root.

### Firmware (Device)

- **[Architecture](../docs/firmware/architecture.md)** — Overall system architecture, IPC topology, platform abstraction
- **[WebRTC Streaming](../docs/firmware/guides/webrtc-streaming.md)** — P2P/SFU modes, MQTT signaling, WHIP/WHEP protocol
- [Push Streaming Protocol](../docs/firmware/api/push-streaming-protocol.md) — Detailed signaling protocol for P2P/SFU
- [IPC Protocol](../docs/firmware/api/ipc-protocol.md) — Inter-process communication protocol
- [mediad API](../docs/firmware/api/mediad-api.md) — Media daemon command and event reference
- [iot_agent API](../docs/firmware/api/iot-agent-api.md) — Cloud agent MQTT interface
- [OSD Elements Config](../docs/firmware/api/osd-elements-config-api.md) — OSD watermark configuration API
- [Triggers Config](../docs/firmware/api/triggers-config-api.md) — Trigger automation configuration
- [Configuration Reference](../docs/firmware/guides/config-reference.md) — All configuration options

## Third-Party Dependencies

NeuroCast uses the following open-source projects. All are statically linked into the firmware binaries.

### Build-Time Dependencies

| Project | Version | License | Usage |
|---------|---------|---------|-------|
| [libzmq](https://github.com/zeromq/libzmq) | 4.3.5 | LGPL-3.0 | IPC message queue (ZeroMQ) |
| [cppzmq](https://github.com/zeromq/cppzmq) | 4.11.0 | MIT | ZeroMQ C++ header-only binding |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | JSON parser |
| [spdlog](https://github.com/gabime/spdlog) | 1.17.0 | MIT | Logging facade |
| [OpenSSL](https://www.openssl.org/) | system | Apache 2.0 | TLS/SSL and cryptography |
| [libcurl](https://curl.se/libcurl/) | system | curl (MIT/X) | HTTP client & file download |
| [Eclipse Paho MQTT](https://www.eclipse.org/paho/) | system | EPL-1.0 / EDL-1.0 | MQTT client |
| [metaRTC](https://github.com/metartc/metaRTC) | 8.0 | MIT | WebRTC engine (P2P + WHIP) |

### Test-Only Dependencies

| Project | Version | License | Usage |
|---------|---------|---------|-------|
| [GoogleTest](https://github.com/google/googletest) | system | BSD-3-Clause | Unit test framework |
| [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) | system | LGPL-2.1+ | Fake HTTP server for tests |

### Compatible Server

| Project | License | Usage |
|---------|---------|-------|
| [SRS](https://github.com/ossrs/srs) | MIT | SFU server for WebRTC relay (WHIP/WHEP) |

> **Note**: libzmq (LGPL-3.0) and libmicrohttpd (LGPL-2.1+) are used via static linking. Per LGPL terms, you may relink these libraries with your own modified versions.

## License

Apache License 2.0. See [LICENSE](LICENSE).

如有问题或合作意向，请发送邮件至 hnngm163@gmail.com。
