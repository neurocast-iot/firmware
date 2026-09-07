// linux-generic backend：文件模拟固件烧写（写入本地文件代替 NAND 分区）
#include "pal/fw_flash.h"

#include <cstdio>
#include <fstream>

namespace pal {

namespace {

const char* SlotName(FwSlot slot) {
    switch (slot) {
        case FwSlot::kSlotA: return "A";
        case FwSlot::kSlotB: return "B";
        case FwSlot::kRecovery: return "recovery";
    }
    return "?";
}

// 模拟槽位文件路径：/tmp/nc_fw_slot_<name>.img
std::string SlotFilePath(FwSlot slot) {
    return std::string("/tmp/nc_fw_slot_") + SlotName(slot) + ".img";
}

class FileFwFlash : public IFwFlash {
public:
    FwSlot GetActiveSlot() override { return FwSlot::kSlotA; }

    bool WriteImage(FwSlot slot, const std::string& image_path,
                    ProgressCallback progress_cb, void* user_data) override {
        std::ifstream src(image_path, std::ios::binary);
        if (!src) {
            std::fprintf(stderr, "[pal.fw.file] cannot open image: %s\n", image_path.c_str());
            return false;
        }
        std::ofstream dst(SlotFilePath(slot), std::ios::binary | std::ios::trunc);
        if (!dst) {
            return false;
        }
        dst << src.rdbuf();
        if (progress_cb) {
            progress_cb(100, user_data);
        }
        std::printf("[pal.fw.file] wrote %s -> slot %s\n", image_path.c_str(), SlotName(slot));
        return dst.good();
    }

    bool VerifySlot(FwSlot slot) override {
        std::ifstream f(SlotFilePath(slot), std::ios::binary);
        return f.good();
    }

    bool SetBootSlot(FwSlot slot) override {
        std::printf("[pal.fw.file] boot slot set to %s\n", SlotName(slot));
        return true;
    }
};

} // namespace

std::unique_ptr<IFwFlash> CreateFwFlash() {
    return std::make_unique<FileFwFlash>();
}

} // namespace pal
