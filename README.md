# NeuroCast

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.20+-blue.svg)](https://cmake.org/)
[![C++14](https://img.shields.io/badge/C++-14-blue.svg)](https://isocpp.org/)

A multi-platform embedded media & IoT camera framework.

NeuroCast provides the framework layer for building IP camera firmware: media services (recording, streaming, OSD watermarks, snapshots), IoT cloud connectivity, trigger-based automation, and OTA updates — all behind clean platform abstractions.

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

## License

Apache License 2.0. See [LICENSE](LICENSE).
