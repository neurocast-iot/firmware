/**
 * @file driver_factory.h
 * @brief 平台驱动工厂（内部头文件，不进公共 include）
 *
 * 编排层（camera_device_impl.cpp）从这里拿具体平台的驱动实例，
 * 自己不 include 任何平台头文件。选哪个平台的实现由 CMake 编译期
 * 分支决定（链接对应平台的 src/<平台>/driver_factory.cpp）。
 *
 * 新增平台 = 新写一个 src/<平台>/driver_factory.cpp 实现
 * createPlatformDriver()，编排层零改动。
 */
#pragma once

#include "camera/camera_driver.h"

#include <memory>

namespace camera {

/**
 * @brief 创建当前编译期平台的 CameraDriver 实例
 *
 * @note 返回的驱动尚未初始化，调用方负责后续
 *       setDispatcher() 和 init()
 */
std::unique_ptr<CameraDriver> createPlatformDriver(const CameraConfig& config);

} // namespace camera
