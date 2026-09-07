/**
 * @file local_upgrade_trigger.cpp
 * @brief 本地升级触发器实现
 */

/**
 * @brief Local upgrade trigger implementation
 */
#include "local_upgrade_trigger.h"
#include "ota_manager.h"
#include "ipc/signal_manager.h"
#include "nc/common/log_utils.h"

#include <ctime>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

namespace iot_agent {

namespace {

/** 扫描 inbox 目录找最新的 .tar.gz 或 .tgz 升级包；返回完整路径，没找到返回空 */
std::string findLatestPackageInInbox(const char* dir) {
    if (!dir) return {};
    DIR* d = ::opendir(dir);
    if (!d) return {};
    std::string latest;
    time_t latest_mtime = 0;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name ? ent->d_name : "";
        if (name.empty() || name == "." || name == "..") continue;
        /* 只认 .tar.gz / .tgz 后缀 */
        const bool is_targz = (name.size() > 7 && name.substr(name.size() - 7) == ".tar.gz");
        const bool is_tgz   = (name.size() > 4 && name.substr(name.size() - 4) == ".tgz");
        if (!is_targz && !is_tgz) continue;
        struct stat st {};
        const std::string full = std::string(dir) + "/" + name;
        if (::stat(full.c_str(), &st) == 0 && st.st_mtime > latest_mtime) {
            latest_mtime = st.st_mtime;
            latest = full;
        }
    }
    ::closedir(d);
    return latest;
}

} // namespace

LocalUpgradeTrigger::LocalUpgradeTrigger(ota::OtaManager& otaManager, const std::string& otaBaseDir)
    : m_otaManager(otaManager)
    , m_otaBaseDir(otaBaseDir) {}

void LocalUpgradeTrigger::poll() {
    auto& sig = SignalManager::instance();
    const int type = sig.pendingUpgradeType();
    if (type == 0) return;
    sig.clearPendingUpgrade();

    /* inbox 路径：{otaBaseDir}/fw/inbox 或 {otaBaseDir}/sw/inbox */
    const std::string typeStr = (type == 2) ? "fw" : "sw";
    const std::string inbox = m_otaBaseDir + "/" + typeStr + "/inbox";

    std::string pkg = findLatestPackageInInbox(inbox.c_str());
    if (!pkg.empty()) {
        m_otaManager.upgradeFromLocalFile(pkg, typeStr, "");
    } else {
        NC_LOGW("[LocalUpgrade] {} inbox 中未找到升级包", inbox.c_str());
    }
}

} // namespace iot_agent
