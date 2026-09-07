# NeuroCast

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.20+-blue.svg)](https://cmake.org/)
[![C++14](https://img.shields.io/badge/C++-14-blue.svg)](https://isocpp.org/)

A multi-platform embedded media & IoT camera framework.

NeuroCast provides the framework layer for building IP camera firmware: **WebRTC real-time streaming** (P2P + SFU), media services (recording, OSD watermarks, snapshots), IoT cloud connectivity, trigger-based automation, and OTA updates — all behind clean platform abstractions.

## Key Features

### WebRTC Real-Time Streaming

Built-in WebRTC support with two streaming modes:

- **P2P** — Direct browser-to-device connection via MQTT signaling, Full-ICE (host + STUN/TURN), lowest latency
- **SFU** — WHIP push to SRS server, WHEP pull by viewers, supports multiple viewers and NAT traversal fallback

The device is always the answerer in P2P mode and the offerer in SFU (WHIP) mode. No automatic mode switching — the frontend controls all transitions via MQTT signaling or RPC.

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

| Directory | Description |
|-----------|-------------|
| `pal/` | Platform Abstraction Layer — pure interfaces + backends |
| `libs/` | Platform-agnostic libraries (camera, recorder, OSD, HTTP, MQTT, etc.) |
| `apps/` | Applications (mediad, iot_agent) |
| `tests/` | Unit tests (x86 + mock backend) |
| `cmake/` | CMake modules and toolchain files |
| `third_party/` | FetchContent dependency declarations |
| `scripts/` | Build and analysis scripts |

## Adding a New Platform

1. Create `pal/backends/<your-platform>/` implementing `pal/include/pal/*.h`
2. Create `libs/camera/src/<your-platform>/` implementing `CameraDriver`
3. Create `libs/recorder/src/<your-platform>/` implementing `recorder_driver` / `demuxer_driver`
4. Add `elseif(NC_PLATFORM STREQUAL "<your-platform>")` branches in `pal/CMakeLists.txt`, `libs/camera/CMakeLists.txt`, `libs/recorder/CMakeLists.txt`
5. Add a CMake preset in `CMakePresets.json`

The apps and libs layers require zero changes.

## What's in the Open-Source Version

| Included | Not Included |
|----------|-------------|
| All framework interfaces (`pal/include/`, `libs/*/include/`) | Vendor SDK backend implementations |
| `linux-generic` software rendering backend | `anyka-av100` (Anychip) backend |
| `mock` backend for testing | Vendor-specific toolchain files |
| All application source code (mediad, iot_agent) | Vendor SDK headers/libraries |
| Full unit test suite | Device-specific deployment configs |
| Trigger system (timer, bluetooth, record) | |
| OSD engine with cross-resolution support | |
| Config sync protocol | |

## Documentation

Full documentation is in the [docs/](../docs/) directory at the repository root.

### Firmware (Device)

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
