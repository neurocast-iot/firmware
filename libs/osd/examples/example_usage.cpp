/**
 * example_usage.cpp - C++ OSD库使用示例
 * 
 * 编译方法：
 *   cd application/lib-osd-cpp
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_TOOLCHAIN_FILE=../../platform/toolchain.cmake
 *   make
 * 
 * 功能说明：
 *   - 展示如何配置日志回调
 *   - 展示如何创建各种OSD元素
 *   - 展示如何刷新和清理
 */

#include "OsdManager.h"
#include "OsdTypes.h"
#include <stdio.h>
#include <unistd.h>

using namespace osd;

int main() {
    printf("=== C++ OSD Library Example ===\n");
    
    // 1. 配置管理器
    ManagerConfig mgr_config;
    mgr_config.channel = 0;
    mgr_config.video_width = 1920;
    mgr_config.video_height = 1080;
    mgr_config.font_file = "/usr/resource/font/OSD.ttf";
    
    // 2. 配置日志回调（将内部日志转发到控制台）
    mgr_config.withLogCallback([](LogLevel level, const std::string& msg) {
        const char* level_str;
        switch (level) {
            case LogLevel::Debug: level_str = "DEBUG"; break;
            case LogLevel::Info:  level_str = "INFO";  break;
            case LogLevel::Warn:  level_str = "WARN";  break;
            case LogLevel::Error: level_str = "ERROR"; break;
            default:              level_str = "UNKNOWN"; break;
        }
        printf("[OSD:%s] %s\n", level_str, msg.c_str());
    });
    
    // 3. 创建管理器（自动初始化SDK）
    OsdManager manager(mgr_config);
    printf("[Example] Manager created\n");
    
    // 3. 添加时间水印
    TimeElementConfig time_cfg;
    time_cfg.at(10, 10)
          .withSize(450, 60)
          .withColor(Color::White())
          .customFormat("%Y-%m-%d %H:%M:%S")
          .showWeek(true);
    
    int time_idx = manager.addTimeElement(time_cfg);
    printf("[Example] Time element added at index %d\n", time_idx);
    
    // 4. 添加文本
    TextElementConfig text_cfg;
    text_cfg.withText("Camera 01")
            .at(10, 80)
            .withFontSize(32)
            .withColor(Color::SkyBlue());
    
    int text_idx = manager.addTextElement(text_cfg);
    printf("[Example] Text element added at index %d\n", text_idx);
    
    // 5. 添加矩形
    RectConfig rect_cfg;
    rect_cfg.at(100, 100)
          .withSize(200, 150)
          .borderWidth(3)
          .borderColor(Color::White())
          .withFill(Color{100, 0, 0, 0});  // 半透明填充
    
    int rect_idx = manager.addRectElement(rect_cfg);
    printf("[Example] Rect element added at index %d\n", rect_idx);
    
    // 6. 添加圆形
    CircleConfig circle_cfg;
    circle_cfg.at(400, 300)
             .withRadius(50)
             .borderWidth(2)
             .borderColor(Color::Red())
             .withFill(Color{100, 255, 0, 0});
    
    int circle_idx = manager.addCircleElement(circle_cfg);
    printf("[Example] Circle element added at index %d\n", circle_idx);
    
    // 7. 添加椭圆
    EllipseConfig ellipse_cfg;
    ellipse_cfg.at(600, 400)
               .withRadius(80, 50)
               .borderWidth(2)
               .borderColor(Color::Green());
    
    int ellipse_idx = manager.addEllipseElement(ellipse_cfg);
    printf("[Example] Ellipse element added at index %d\n", ellipse_idx);
    
    printf("[Example] Total elements: %d\n", manager.getElementCount());
    
    // 8. 主循环：刷新时间
    printf("[Example] Starting update loop (press Ctrl+C to stop)...\n");
    for (int i = 0; i < 5; i++) {
        sleep(1);
        manager.update();
        printf("[Example] Update #%d\n", i + 1);
    }
    
    // 9. 清理（析构函数自动调用）
    printf("[Example] Example completed\n");
    
    return 0;
}
