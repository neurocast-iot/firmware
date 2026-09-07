/**
 * @file platform_factory.cpp
 * @brief 平台工厂实现 —— 编译期裁剪
 *
 * CMake 通过 IOT_AGENT_PLATFORM 选项定义 PLATFORM_XXX 宏，
 * 只有选中的平台代码会编进二进制，其他平台的 #include 和 new 全部跳过。
 *
 * 运行时做一次名字校验：配置里的 platform 字段必须和编译时选的一致，
 * 不一致就报错退出（防止刷错固件）。
 */
#include "platform_factory.h"

#include "nc/common/log_utils.h"

/* ---- 编译期裁剪：只 include 选中的平台头文件 ---- */
#if defined(PLATFORM_THINGSBOARD)
#include "cloud/thingsboard/tb_adapter.h"
#elif defined(PLATFORM_AWS)
/* #include "cloud/aws/aws_adapter.h" */
#elif defined(PLATFORM_EMQX)
/* #include "cloud/emqx/emqx_adapter.h" */
#endif

namespace iot_agent {

std::unique_ptr<CloudAdapter> PlatformFactory::create(const std::string& name) {
#if defined(PLATFORM_THINGSBOARD)
    /* 编译的是 TB，配置也必须写 TB */
    if (name == "thingsboard" || name == "tb") {
        NC_LOGI("[PlatformFactory] create TBAdapter");
        return std::unique_ptr<CloudAdapter>(new TBAdapter());
    }
    NC_LOGE("[PlatformFactory] firmware built for thingsboard, but config says: {}", name.c_str());
    return nullptr;

#elif defined(PLATFORM_AWS)
    if (name == "aws" || name == "aws_iot") {
        NC_LOGI("[PlatformFactory] create AwsAdapter");
        /* return std::unique_ptr<CloudAdapter>(new AwsAdapter()); */
    }
    NC_LOGE("[PlatformFactory] firmware built for aws, but config says: {}", name.c_str());
    return nullptr;

#elif defined(PLATFORM_EMQX)
    if (name == "emqx") {
        NC_LOGI("[PlatformFactory] create EmqxAdapter");
        /* return std::unique_ptr<CloudAdapter>(new EmqxAdapter()); */
    }
    NC_LOGE("[PlatformFactory] firmware built for emqx, but config says: {}", name.c_str());
    return nullptr;

#else
    /* 没定义任何平台宏 → 编译配置有问题 */
    NC_LOGE("[PlatformFactory] no platform compiled in! check IOT_AGENT_PLATFORM in CMakeLists.txt");
    return nullptr;
#endif
}

} // namespace iot_agent
