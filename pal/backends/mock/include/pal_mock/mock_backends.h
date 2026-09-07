// mock backend：单元测试桩（记录调用，供断言）
// 测试代码链接 nc::pal_mock 并直接实例化 Mock* 类
#pragma once

#include <vector>

#include "pal/fw_flash.h"
#include "pal/osd_backend.h"
#include "pal/system_info.h"

namespace pal {
namespace mock {

// 全部操作成功并计数，供测试断言调用次数（对齐 IOsdBackend 新接口：
// 画布/文字/提交分离，字体与颜色归后端记住）
class MockOsdBackend : public IOsdBackend {
public:
    int init_calls = 0;
    int draw_calls = 0;       ///< DrawBitmap 成功次数
    int draw_str_calls = 0;   ///< DrawWideStr 成功次数
    int commit_calls = 0;     ///< Commit 次数

    bool Init(const uint32_t[16]) override { ++init_calls; return true; }
    void Destroy() override {}
    bool CreateCanvas(const OsdCanvas&) override { return true; }
    void DestroyCanvas(int, int) override {}
    /* 上限返回固定大值：mock 不模拟硬件限制，测试自行钳位 */
    bool GetMaxRect(int, int& maxWidth, int& maxHeight) override {
        maxWidth = 1920;
        maxHeight = 1080;
        return true;
    }
    /* mock 不模拟区域数量限制（真机 anyka 为 4），给宽松值 */
    int GetMaxRegions() const override { return 16; }
    int DrawBitmap(int, int, int, int, const uint8_t*, size_t) override {
        ++draw_calls;
        return 0;
    }
    void SetFontFile(int, const std::string&) override {}
    void SetFontSize(int, int) override {}
    void SetDrawColor(int, int) override {}
    int DrawWideStr(int, int, int, int, const uint16_t*, int) override {
        ++draw_str_calls;
        return 0;
    }
    int Commit() override { ++commit_calls; return 0; }
};

class MockFwFlash : public IFwFlash {
public:
    FwSlot active_slot = FwSlot::kSlotA;
    FwSlot boot_slot = FwSlot::kSlotA;
    int write_calls = 0;
    bool write_result = true;

    FwSlot GetActiveSlot() override { return active_slot; }
    bool WriteImage(FwSlot, const std::string&, ProgressCallback cb, void* ud) override {
        ++write_calls;
        if (cb) cb(100, ud);
        return write_result;
    }
    bool VerifySlot(FwSlot) override { return write_result; }
    bool SetBootSlot(FwSlot slot) override { boot_slot = slot; return true; }
};

class MockSystemInfo : public ISystemInfo {
public:
    std::string GetPlatformName() override { return "mock"; }
    std::string GetFirmwareVersion() override { return "0.0.0-mock"; }
    std::string GetDeviceId() override { return "mock-device"; }
    bool GetSocTemperature(float& celsius) override { celsius = 42.0f; return true; }
};

} // namespace mock
} // namespace pal
