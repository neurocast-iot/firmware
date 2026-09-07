# lib-osd-cpp - C++ OSD库 (嵌入式优化版)

## 概述

面向AK3918AV100嵌入式平台的C++ OSD库,采用面向对象架构,提供类型安全、内存安全的OSD元素管理。

## 特性

- ✅ **零侵入**: 完全独立于现有C版本osd.c,不修改任何现有代码
- ✅ **嵌入式优化**: C++14标准,禁用RTTI/异常,静态内存分配
- ✅ **类型安全**: enum class强类型枚举,编译期检查
- ✅ **内存安全**: RAII资源管理,零动态分配,无内存泄漏风险
- ✅ **架构清晰**: 分层设计(Manager → Element → Utils → SDK)
- ✅ **易于扩展**: 开闭原则,添加新元素类型无需修改现有代码

## 架构设计

```
应用层 (iot_live等)
    ↓
OsdManager (管理器 - 元素调度、SDK生命周期)
    ↓
OsdElement派生体系 (Time/Text/Rect/Circle/Ellipse)
    ↓
OsdUtils (内联工具函数 - 颜色匹配、字符转换)
    ↓
Anyka SDK (ak_osd_ex_*)
```

## 编译

```bash
cd application/lib-osd-cpp
mkdir build && cd build
cmake .. -DCMAKE_CXX_COMPILER=arm-anycloud-linux-uclibcgnueabi-g++
make
```

输出: `build/libosd_cpp.a` (约35KB)

## 使用示例

```cpp
#include "OsdManager.h"
#include "OsdTypes.h"

using namespace osd;

// 1. 配置管理器
ManagerConfig config;
config.channel = 0;
config.video_width = 1920;
config.video_height = 1080;
config.font_file = "/usr/resource/font/OSD.ttf";

// 2. 创建管理器
OsdManager manager(config);

// 3. 添加时间水印
TimeElementConfig time_cfg;
time_cfg.at(10, 10)
      .withSize(450, 60)
      .withColor(Color::White())
      .customFormat("%Y-%m-%d %H:%M:%S")
      .showWeek(true);
manager.addTimeElement(time_cfg);

// 4. 添加文本
TextElementConfig text_cfg;
text_cfg.withText("Camera 01")
        .at(10, 80)
        .withFontSize(32)
        .withColor(Color::SkyBlue());
manager.addTextElement(text_cfg);

// 5. 添加矩形
RectConfig rect_cfg;
rect_cfg.at(100, 100)
      .withSize(200, 150)
      .borderWidth(3)
      .borderColor(Color::White())
      .withFill(Color{100, 0, 0, 0});
manager.addRectElement(rect_cfg);

// 6. 刷新时间
manager.update();
```

## 资源占用

| 指标 | 数值 |
|------|------|
| RAM占用 | ~200KB (含128KB像素缓冲区) |
| Flash体积 | ~35KB (strip后) |
| 动态分配 | 零 (静态数组+预分配) |
| 虚函数开销 | 零 (type枚举+switch分发) |

## 文件结构

```
lib-osd-cpp/
├── CMakeLists.txt
├── include/
│   ├── OsdTypes.h          # 类型定义 (Color, Config等)
│   ├── OsdElement.h        # 元素基类和派生类声明
│   ├── OsdManager.h        # 管理器接口
│   └── OsdUtils.h          # 内联工具函数
├── src/
│   ├── OsdManager.cpp      # 管理器实现
│   ├── TimeElement.cpp     # 时间元素
│   ├── TextElement.cpp     # 文本元素
│   ├── RectElement.cpp     # 矩形元素
│   ├── CircleElement.cpp   # 圆形元素
│   └── EllipseElement.cpp  # 椭圆元素
└── examples/
    └── example_usage.cpp   # 使用示例
```

## 与C版本对比

| 维度 | C版本 (osd.c) | C++版本 (lib-osd-cpp) |
|------|--------------|----------------------|
| 类型安全 | 弱类型枚举 | enum class强类型 |
| 内存管理 | 手动calloc/free | RAII+静态数组 |
| 扩展性 | 修改大结构体 | 添加新Element文件 |
| 代码组织 | 单文件1000行+ | 模块化分层 |
| API清晰度 | 函数参数10+ | Builder模式 |
| 资源泄漏 | 高风险 | 零风险 |

## 技术栈

- **C++标准**: C++14
- **编译选项**: `-fno-rtti -fno-exceptions -Os`
- **目标平台**: AK3918AV100 (ARM, uClibc)
- **依赖**: Anyka SDK (ak_osd, ak_osd_ex)

## 注意事项

1. **SDK头文件顺序**: 必须确保`ak_common.h`在`ak_osd.h`之前包含
2. **单线程环境**: SDK引用计数使用普通int(非atomic)
3. **字体文件**: 需要在配置中指定正确的字体路径
4. **元素数量**: 最多10个元素(硬件限制)

## 许可证

内部项目,遵循项目许可协议。
