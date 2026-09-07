// iot_monitor 入口（骨架版）
// TODO(第2步): 迁入现有 iot_monitor 主流程（进程/磁盘监控采集循环）
#include "nc/common/log_utils.h"
#include "pal/system_info.h"

int main() {
    nc::common::LogInit("", nc::common::LogLevel::kInfo);
    NC_LOGI("iot_monitor skeleton starting");

    // 骨架自检：通过 pal 接口访问平台探针（依赖方向验证）
    auto sys = pal::CreateSystemInfo();
    NC_LOGI("platform = {}, device = {}",
            sys->GetPlatformName().c_str(), sys->GetDeviceId().c_str());

    float temp = 0.0f;
    if (sys->GetSocTemperature(temp)) {
        NC_LOGI("soc temperature = %.1f C", temp);
    }

    NC_LOGI("iot_monitor skeleton exiting (TODO: migrate real logic)");
    return 0;
}
