/**
 * @file driver_factory.h
 * @brief 平台驱动工厂（内部头文件，不进公共 include）
 *
 * 编排层（mp4_recorder.cpp / mp4_demuxer.cpp）从这里拿具体平台的驱动实例，
 * 自己不 include 任何平台头文件。
 *
 * 新增平台 = 新写一个 src/<平台>/driver_factory.cpp 实现这两个工厂函数，
 * 编排层零改动。
 */
#pragma once

#include "recorder/recorder_driver.h"
#include "recorder/demuxer_driver.h"

#include <memory>

namespace recorder {

/**
 * 创建当前平台的录像驱动实例
 *
 * 编译期由 CMake 决定链接哪个平台的 driver_factory.cpp，
 * 运行时直接 new 对应平台的实现，零开销。
 */
std::unique_ptr<RecorderDriver> createRecorderDriver();

/**
 * 创建当前平台的解封装驱动实例
 *
 * 编译期由 CMake 决定链接哪个平台的 driver_factory.cpp，
 * 运行时直接 new 对应平台的实现，零开销。
 */
std::unique_ptr<DemuxerDriver> createDemuxerDriver();

} // namespace recorder
