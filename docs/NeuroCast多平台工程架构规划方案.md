# NeuroCast 多平台工程架构规划方案

> 版本：v1.0
> 日期：2026-07-28
> 状态：方案已确认，待实施
> 范围：ota_agent、iot_monitor、lib-osd-cpp、lib-exporter、lib-mq 五个模块的工程化重组，以及面向多平台的长期架构

---

## 1. 项目命名

| 项目 | 命名 |
|------|------|
| 项目名称 | **NeuroCast**（Neuro 神经网络 + Cast 推流广播） |
| 应用 monorepo 仓库名 | `neurocast` |
| CMake target 命名空间 | `nc::`（如 `nc::mq`、`nc::osd`、`nc::exporter`） |
| CMake 变量前缀 | `NC_`（如 `NC_PLATFORM`、`NC_VENDOR_SDK_ROOT`） |

命名理由：语义直指核心业务（WebRTC 实时推流），"Neuro" 预留 AI/NPU 能力演进空间（厂商 SDK V1.10 已引入 NPU 与 AI 检测库），派生命名体系无冲突。

---

## 2. 背景与问题诊断

### 2.1 现状（gw_av100 仓库）

| 指标 | 数值 | 问题 |
|------|------|------|
| git 跟踪文件总数 | 132,138 个 | 仓库臃肿 |
| 其中 os/（内核+U-Boot） | 76,784 个（58%） | 厂商代码占据主体 |
| 其中 third_party/ | 38,704 个（29%） | 开源库源码入库 |
| 自有 application/ | 11,595 个（8.8%） | 核心资产占比不足一成 |
| .git 体积 | 2.7 GB | clone/操作缓慢 |

项目本质是"厂商 PDK 原地开发"模式：git 首提交为 AnyCloud39AV100 PDK V1.03 基线，后原地升级至 SDK V1.08，此后所有业务开发均在 SDK 源码树内进行。

### 2.2 五个模块的现存工程问题

1. **工具链文件重复**：每个模块各自维护一份 `cmake/arm-toolchain.cmake`（共 5+ 份），修改工具链需改多处。
2. **基础代码重复**：`log_utils`、`file_utils` 在 ota_agent 与 iot_monitor 中各自实现。
3. **构建产物入库**：各模块 `build-arm/`（含 .a、可执行文件、CMakeCache）、`logs/` 混在源码树中。
4. **无统一构建入口**：各模块独立编译，无法一条命令构建全部产物。
5. **平台耦合未隔离**：Anyka SDK 调用散布在业务代码中，无法迁移到其他平台。

---

## 3. 架构总原则：三层分离 + 制品化依赖

参照业界主流做法（Yocto SDK / Buildroot external / Spring Boot 依赖模型）：

```
┌─────────────────────────────────────────────────┐
│  应用层（NeuroCast monorepo）—— 日常开发主战场      │
│  纯业务代码 + 依赖声明，不含任何厂商/系统源码        │
└──────────────△──────────────────────────────────┘
               │ 消费版本化制品（SDK 头文件+预编译库）
┌──────────────┴──────────────────────────────────┐
│  平台层（厂商 SDK / 内核 / U-Boot）—— 低频变更     │
│  独立仓库，vendor branch 管理厂商升级              │
└─────────────────────────────────────────────────┘
```

核心规则：
- 应用开发者像使用 Spring Boot 依赖一样消费平台层，源码级互不可见；
- 两层之间唯一接口 = 一个 SDK 版本号 + 一份头文件/库制品包；
- 构建产物（build/、logs/、image/）一律不入 git。

---

## 4. 模块平台耦合度分析（迁移依据）

| 模块 | 性质 | 平台耦合度 | 说明 |
|------|------|-----------|------|
| lib-mq | ZeroMQ 消息封装库 | ⚪ 无耦合 | 纯 POSIX，任何 Linux 平台可用 |
| lib-exporter | 指标导出库 | ⚪ 无耦合 | 纯逻辑 + 网络 |
| iot_monitor | 进程/磁盘监控应用 | 🟡 弱耦合 | 采集基于 /proc、statvfs 等通用接口 |
| ota_agent | OTA 升级应用 | 🟡 中耦合 | 下载/校验/manifest 通用；fw_handler（NAND 烧写）平台特定 |
| lib-osd-cpp | OSD 水印库 | 🔴 强耦合 | 直接调用 ak_osd_ex_* 系列 Anyka SDK |

---

## 5. 目标目录结构

```
neurocast/                            # 应用 monorepo（独立 git 仓库）
├── CMakeLists.txt                    # 顶层：add_subdirectory 编排全部模块
├── CMakePresets.json                 # 平台预设：一条命令切换目标平台
├── cmake/
│   ├── toolchains/                   # ★ 工具链集中管理（消除多份重复）
│   │   ├── arm-ak3918.cmake          #    Anyka AV100
│   │   ├── x86-linux.cmake           #    宿主机（开发/单元测试）
│   │   └── <future-platform>.cmake   #    新平台按需新增
│   └── modules/                      #    Find*.cmake、公共编译选项
│
├── pal/                              # ★ 平台抽象层（多平台核心）
│   ├── include/pal/                  #    纯接口，零平台依赖
│   │   ├── osd_backend.h             #    IOsdBackend：位图绘制/色表设置
│   │   ├── fw_flash.h                #    IFwFlash：固件分区读写
│   │   └── system_info.h             #    ISystemInfo：平台探针
│   └── backends/
│       ├── anyka-av100/              #    唯一允许 #include <ak_*.h> 的目录
│       ├── linux-generic/            #    x86：文件模拟烧写、软件渲染 OSD
│       └── mock/                     #    单元测试桩
│
├── libs/                             # 通用库（只依赖 pal 接口和彼此）
│   ├── common/                       # ★ 新增：log_utils/file_utils 收编于此
│   ├── mq/                           #    ← lib-mq 迁入，target: nc::mq
│   ├── exporter/                     #    ← lib-exporter 迁入，target: nc::exporter
│   └── osd/                          #    ← lib-osd-cpp 拆分后迁入，target: nc::osd
│       ├── include/osd/              #      元素模型/布局逻辑（通用）
│       └── src/                      #      渲染调用走 pal::IOsdBackend
│
├── apps/                             # 应用（只依赖 libs，不直接接触 SDK）
│   ├── ota_agent/                    #    fw_handler 改为调 pal::IFwFlash
│   └── iot_monitor/
│
├── third_party/                      # 开源依赖：仅 CMake FetchContent 声明
│   └── deps.cmake                    #    zeromq/spdlog/curl 按版本拉取，源码不入库
│
├── scripts/                          # 打包、部署、OTA 制包脚本
├── build/                            # 全部构建产物（gitignore）
│   ├── arm-ak3918-release/
│   └── x86-debug/
└── .gitignore                        # build/、logs/ 全量排除
```

### 5.1 依赖方向（严格单向）

```
apps  ──→  libs  ──→  pal 接口
                        △
             ┌──────────┼──────────┐
        anyka后端    linux后端    mock后端
             │
        厂商 SDK 头文件/库（仅此一处引用）
```

**铁律：`apps/` 与 `libs/` 中永远不出现 `#include <ak_xxx.h>`。**
平台差异全部收敛于 `pal/backends/`，新增平台 = 新增一个 backend 目录 + 一个 toolchain 文件 + 一个 preset 条目，其余代码零修改（开闭原则）。

### 5.2 平台切换体验（CMakePresets）

```bash
# 板端固件
cmake --preset arm-ak3918 && cmake --build --preset arm-ak3918
# 宿主机单元测试
cmake --preset x86-debug && ctest --preset x86-debug
```

preset 内部绑定工具链文件 + `NC_PLATFORM` 变量，顶层 CMake 据此选择编译对应 backend。

### 5.3 与厂商 SDK 的衔接

`cmake/toolchains/arm-ak3918.cmake` 通过 `NC_VENDOR_SDK_ROOT` 变量指向 SDK 的 include/lib 位置：
- 过渡期：指向现有 `gw_av100/platform/` 目录；
- 目标态：指向制品化的 SDK 安装包（版本号锁定），应用仓库对 SDK 升级无感知。

---

## 6. 各模块迁移改造点

| 模块 | 迁移动作 | 工作量 |
|------|---------|-------|
| lib-mq | 原样迁入 `libs/mq/`，删除自带 toolchain，target 改名 `nc::mq` | 小 |
| lib-exporter | 原样迁入 `libs/exporter/`，target 改名 `nc::exporter` | 小 |
| 公共代码 | 从 ota_agent/iot_monitor 抽出 log_utils、file_utils → `libs/common/` | 小 |
| lib-osd-cpp | 拆分：元素/布局逻辑留 `libs/osd/`；OsdSdkManager 中的 ak_osd_* 调用下沉为 `pal/backends/anyka-av100/osd_backend_anyka.cpp` | **中**（唯一需要真正重构的模块） |
| ota_agent | fw_handler 烧写逻辑下沉为 pal::IFwFlash 的 anyka 实现；sw 升级逻辑通用，不动 | 中小 |
| iot_monitor | 基本原样迁入；Anyka 特定探针（如有）走 pal::ISystemInfo | 小 |

---

## 7. 实施路线（渐进式，每步可独立验证、可回退）

| 阶段 | 内容 | 验证标准 |
|------|------|---------|
| **第 1 步** | 建立 `neurocast` 仓库骨架（顶层 CMake + CMakePresets + 集中 toolchain），迁入 lib-mq、lib-exporter 两个零耦合库 | ARM 与 x86 双平台编译通过 |
| **第 2 步** | 抽取 `libs/common/`；迁入 iot_monitor、ota_agent（fw_handler 暂直连 SDK，标记 TODO） | ARM 产物与现版本设备端行为一致 |
| **第 3 步** | 建立 `pal/` 层；重构 lib-osd-cpp 为接口 + anyka backend；ota_agent 的 fw_handler 下沉至 pal | OSD 水印、OTA 烧写设备端回归通过 |
| **第 4 步** | 补 mock backend 与 x86 单元测试 | 新功能可先宿主机验证再上板 |
| **未来** | 新平台接入 | 仅新增 toolchain + preset + backend 三件事 |

全程旧目录（`gw_av100/application/`）保持不动，新仓库产物在设备端验证通过后再废弃对应旧目录。

---

## 8. 厂商 SDK 层的配套策略（附）

应用层迁出后，`gw_av100` 仓库回归"平台仓库"定位，SDK 升级采用 **vendor branch** 模式：

```
vendor 分支（只放厂商原始交付物，一版本一 commit + tag）
  ●───────●───────●
  v1.03    v1.08   v1.10（厂商已交付，待导入）
   \        \       \
    \        \       merge ← 升级 = 将 vendor 合入 main
     \        \      ↓
main 分支 ●────●────●────●
（vendor 基线 + 本地修改：U-Boot AB 启动、NAND 分区等）
```

升级流程：vendor 分支导入新版 → tag `vendor/vX.XX` → merge 入 main → 冲突点即本地修改点，逐个适配 → 设备端回归 → tag `vX.XX-local.N`。本地修改永不丢失。

已知事实（2026-07 核实）：
- 当前项目基线为 SDK V1.08（2023-09-11 发布）；
- 厂商已交付 V1.10（2024-09-14 发布），位于 `/mnt/e/workspace/c++/AnyCloud39AV100_SDK_V1.10`；
- V1.10 新增：VIDEO_DEV2/DEV3、VIDEO_CHN4~7/18/19、`ak_vi_load_sensor_cfg_ex`（日夜模式 ISP 子配置）、NPU（ak_npu.h、libak_nna/nne）、哭声检测（libakAIcrydetect）；
- 风险提示：platform/lib 预编译库可能依赖对应版本内核驱动接口，"只换库不换内核"存在 ABI 不兼容风险，升级时需整体基线验证。

---

## 9. 待决策事项

| 事项 | 选项 | 状态 |
|------|------|------|
| 项目命名 | NeuroCast（推荐）/ SynapStream / AetherLink / LumiRTC | 待确认 |
| neurocast 仓库位置 | 独立 git 仓库（推荐）/ 当前仓库内新顶层目录过渡 | 待确认 |
| 实施启动 | 从第 1 步（骨架 + 零耦合库迁入）开始 | 待确认 |
