#include "ota_manager.h"

#include "local_upgrade_trigger.h"
#include "nc/common/crypto_utils.h"
#include "nc/common/file_utils.h"
#include "nc/common/log_utils.h"
#include "nc/common/string_utils.h"

#include "fw/fw_handler.h"
#include "sw/sw_handler.h"

#include "cloud/cloud_service.h"

#include "nc/http/download.h"

#include "ota_result_reporter.h"

#include "cJSON.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace iot_agent {
namespace ota {

namespace {

} // namespace

OtaManager::OtaManager() = default;

OtaManager::~OtaManager() {
    stop();
}

// 初始化：保存 CloudService 指针，创建 OtaResultReporter 和 LocalUpgradeTrigger
bool OtaManager::initialize(CloudService* cloud, const std::string& base_dir) {
    m_cloud = cloud;
    m_reporter = std::make_unique<OtaResultReporter>(cloud);
    m_base_dir = base_dir.empty() ? OtaPaths::OTA_BASE : base_dir;
    m_localTrigger = std::make_unique<LocalUpgradeTrigger>(*this, m_base_dir);
    m_stop.store(false);
    NC_LOGI("[OTA] Manager 初始化完成 base_dir={}", m_base_dir.c_str());
    return true;
}

void OtaManager::stop() {
    if (m_stop.exchange(true)) return;
    // 等待 worker 线程结束
    if (m_worker && m_worker->joinable()) {
        m_worker->join();
    }
    m_worker.reset();
    m_localTrigger.reset();
    NC_LOGI("[OTA] Manager 已停止");
}

bool OtaManager::isBusy() const {
    return m_busy.load();
}

void OtaManager::pollLocalUpgrade() {
    if (m_localTrigger) {
        m_localTrigger->poll();
    }
}

// 收到 MQTT OTA 通知时调用 —— 直接异步处理，不阻塞 MQTT 回调线程
void OtaManager::onOtaNotice(const OtaNotice& notice) {
    // 检查是否正在升级
    if (m_busy.load()) {
        NC_LOGW("[OTA] 当前正在升级，忽略新通知");
        return;
    }

    Notice n;
    n.type = parseOtaType(notice.type);
    n.source = OtaSource::REMOTE;  // 云端推送
    n.title = notice.title;
    n.version = notice.version;
    n.url = nc::common::TrimString(notice.url);
    n.checksum = notice.checksum;
    n.checksum_algorithm = notice.checksumAlgorithm;
    n.size = static_cast<uint64_t>(notice.size);

    if (n.url.empty()) {
        NC_LOGW("[OTA] onOtaNotice: url 为空，忽略");
        return;
    }
    NC_LOGI("[OTA] 收到通知 type={} version={} url={}",
            otaTypeName(n.type), n.version.c_str(), n.url.c_str());

    // 启动异步线程处理
    m_worker = std::make_unique<std::thread>([this, n]() {
        m_busy.store(true);
        processNotice(n);
        m_busy.store(false);
    });
    m_worker->detach();  // 分离线程，让它独立运行
}

// 本地升级入口（信号触发），跳过下载
bool OtaManager::upgradeFromLocalFile(const std::string& package_path,
                                      const std::string& type,
                                      const std::string& version) {
    if (!nc::common::FileExists(package_path)) {
        NC_LOGE("[OTA] 本地升级包不存在 path={}", package_path.c_str());
        return false;
    }

    // 检查是否正在升级
    if (m_busy.load()) {
        NC_LOGW("[OTA] 当前正在升级，忽略本地升级请求");
        return false;
    }

    Notice notice;
    notice.type = parseOtaType(type);
    notice.source = OtaSource::LOCAL;  // 本地升级
    notice.title = version.empty() ? "local" : version;
    notice.version = version.empty() ? "local" : version;
    notice.url = package_path;
    notice.checksum.clear();  // 本地升级暂不校验 SHA256

    NC_LOGI("[OTA] 本地升级 type={} version={} path={}",
            otaTypeName(notice.type), notice.version.c_str(), package_path.c_str());

    // 启动异步线程处理
    m_worker = std::make_unique<std::thread>([this, notice]() {
        m_busy.store(true);
        processNotice(notice);
        m_busy.store(false);
    });
    m_worker->detach();  // 分离线程，让它独立运行
    return true;
}

void OtaManager::processNotice(Notice notice) {
    // 1) 工作目录
    if (!nc::common::MakeDirs(m_base_dir)) {
        const std::string err = "创建目录失败: " + m_base_dir;
        NC_LOGE("[OTA] {}", err.c_str());
        reportState(notice, "FAILED", err);
        return;
    }
    const std::string dir = packageDir(notice);
    if (!nc::common::MakeDirs(dir)) {
        const std::string err = "创建目录失败: " + dir;
        NC_LOGE("[OTA] {}", err.c_str());
        reportState(notice, "FAILED", err);
        return;
    }

    // 2) 判断是本地包还是 URL：本地包直接用，URL 要下载
    std::string pkgPath = notice.url;
    if (notice.source == OtaSource::REMOTE) {
        // 远程升级：下载
        std::string err;
        if (!downloadPackage(notice, dir, pkgPath, err)) {
            NC_LOGE("[OTA] {}", err.c_str());
            reportState(notice, "FAILED", err);
            return;
        }
    } else {
        // 本地升级：直接用本地路径
        NC_LOGI("[OTA] 本地包路径 pkg={}", pkgPath.c_str());
    }

    // 3) 核心升级（校验 → 执行 → 上报 → 写版本）
    executeUpgradeCore(notice, pkgPath, /*skip_version_dedup=*/false);
}

// 核心升级：版本去重 → SHA256 校验 → 执行 → 写版本 → 上报 → 延迟重启
bool OtaManager::executeUpgradeCore(const Notice& notice, const std::string& pkgPath, bool skip_version_dedup) {
    // 1) 版本去重：避免重启后重复触发
    if (!skip_version_dedup) {
        const std::string installed = readInstalledVersion(notice);
        if (!notice.version.empty() && !installed.empty() && installed == notice.version) {
            NC_LOGI("[OTA] 版本 {} 已安装，跳过", notice.version.c_str());
            reportState(notice, "UPDATED");
            return true;
        }
    }

    // 2) SHA256 校验
    if (!notice.checksum.empty()) {
        std::string err;
        if (!verifyPackageChecksum(notice, pkgPath, err)) {
            NC_LOGE("[OTA] 校验失败: {}", err.c_str());
            reportState(notice, "FAILED", err);
            return false;
        }
    } else {
        NC_LOGW("[OTA] 未提供 checksum，跳过完整性校验");
    }

    // 3) 上报 UPDATING
    reportState(notice, "UPDATING");

    // 4) 执行升级
    bool ok = false;
    std::string err;
    std::string out_version;
    if (notice.type == OtaPackageType::SOFTWARE) {
        // 软件升级：SwHandler 走完整流程（解压→备份→安装→校验→失败回滚）
        SwHandler h(m_base_dir);
        const std::string staging = packageDir(notice) + "/staging/" + notice.version;
        ok = h.executeUpgrade(pkgPath, staging, out_version, err);
    } else {
        // 固件升级：FwHandler 替换 /usr 下的系统文件，流程和 SwHandler 一样（都继承 UpgradeHandlerBase）
        FwHandler h(m_base_dir);
        const std::string staging = OtaPaths::fwStaging(m_base_dir) + "/" + notice.version;
        ok = h.executeUpgrade(pkgPath, staging, out_version, err);
    }

    if (!ok) {
        NC_LOGE("[OTA] 升级执行失败: {}", err.c_str());
        reportState(notice, "FAILED", err);
        return false;
    }

    // 5) 写版本文件
    if (!writeTextAtomic(currentVersionPath(notice), notice.version + "\n")) {
        NC_LOGW("[OTA] 写版本文件失败 path={}", currentVersionPath(notice).c_str());
    }

    // 6) 上报 UPDATED
    reportState(notice, "UPDATED");

    // 7) 延迟重启（fork 子进程执行 reboot，不阻塞 worker）
    NC_LOGI("[OTA] 升级成功，{}s 后重启...", OtaConstants::REBOOT_DELAY_SEC);
    scheduleReboot();
    return true;
}

bool OtaManager::verifyPackageChecksum(const Notice& notice, const std::string& filePath, std::string& outErr) {
    if (notice.checksum.empty()) {
        return true;  // 没提供就跳过
    }
    NC_LOGI("[OTA] 校验 SHA256 type={} expected={}",
            otaTypeName(notice.type), notice.checksum.c_str());
    std::string got = nc::common::CalculateFileSHA256(filePath);
    if (got.empty()) {
        outErr = "SHA256 计算失败";
        return false;
    }
    if (got != notice.checksum) {
        outErr = "SHA256 不一致 expected=" + notice.checksum + " got=" + got;
        return false;
    }
    reportState(notice, "VERIFIED");
    return true;
}

bool OtaManager::downloadPackage(const Notice& notice, const std::string& outDir,
                                 std::string& outPath, std::string& outErr) {
    if (!nc::common::MakeDirs(outDir)) {
        outErr = "创建下载目录失败: " + outDir;
        return false;
    }

    // 文件名：从 URL 取
    std::string fileName = nc::common::ExtractFileNameFromUrl(notice.url);
    if (fileName.empty() || fileName == "ota") {
        const std::string t = notice.title.empty() ? "ota" : notice.title;
        const std::string v = notice.version.empty() ? "unknown" : notice.version;
        fileName = std::string(otaPrefix(notice.type)) + "_" + t + "_v" + v + ".bin";
    }

    const std::string partPath = outDir + "/" + fileName + ".part";
    const std::string finalPath = outDir + "/" + fileName;

    reportState(notice, "DOWNLOADING");

    // 通用下载走 nc::http::downloadWithPolicy（HttpClient + 重试 + 磁盘检查）
    nc::http::DownloadPolicy policy;
    policy.max_retries = OtaConstants::MAX_RETRY;
    policy.retry_delay_sec = OtaConstants::RETRY_DELAY_SEC;
    policy.timeout_sec = OtaConstants::DOWNLOAD_TIMEOUT_SEC;
    policy.max_file_size = OtaConstants::MAX_FILE_SIZE;
    policy.resume_support = true;

    auto r = nc::http::downloadWithPolicy(
        notice.url, partPath, policy,
        [&notice](int64_t downloaded, int64_t total, void* /*ud*/) -> bool {
            /* 进度上报：先简单打日志，后续可改成 telemetry */
            NC_LOGD("[OTA] 下载进度 {}/{} type={}",
                    downloaded, total, otaTypeName(notice.type));
            return true;
        });

    if (!r.ok) {
        outErr = "下载失败: " + r.errorMsg;
        return false;
    }

    // .part → 正式文件名
    if (::rename(partPath.c_str(), finalPath.c_str()) != 0) {
        outErr = "rename 失败: " + partPath + " errno=" + std::to_string(errno);
        return false;
    }
    outPath = finalPath;
    reportState(notice, "DOWNLOADED");
    NC_LOGI("[OTA] 下载完成 path={} attempts={} bytes={}",
            finalPath.c_str(), r.attempts, r.bytes);
    return true;
}

// 状态上报：委托给 OtaResultReporter
void OtaManager::reportState(const Notice& notice, const char* state, const std::string& err) {
    if (!m_reporter || !state) return;
    m_reporter->reportState(notice.type, notice.title, notice.version, state, err);
}

std::string OtaManager::currentVersionPath(const Notice& notice) const {
    return (notice.type == OtaPackageType::SOFTWARE)
               ? OtaPaths::CURRENT_SW_VERSION
               : OtaPaths::CURRENT_FW_VERSION;
}

std::string OtaManager::readInstalledVersion(const Notice& notice) const {
    std::string content;
    if (!nc::common::ReadFile(currentVersionPath(notice), content)) {
        return "";
    }
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }
    return content;
}

std::string OtaManager::packageDir(const Notice& notice) const {
    return (notice.type == OtaPackageType::SOFTWARE)
               ? (m_base_dir + "/sw")
               : (m_base_dir + "/fw");
}

bool OtaManager::writeTextAtomic(const std::string& path, const std::string& content) const {
    return nc::common::WriteFileAtomic(path, content);
}

// 升级成功 → 延迟重启
// 用 fork 让子进程在后台 sleep + reboot，不阻塞当前 worker
void OtaManager::scheduleReboot() {
    const pid_t pid = ::fork();
    if (pid < 0) {
        NC_LOGE("[OTA] fork 失败，无法触发重启");
        return;
    }
    if (pid == 0) {
        // 子进程：延迟 → reboot
        ::sleep(OtaConstants::REBOOT_DELAY_SEC);
        NC_LOGI("[OTA] 触发系统重启");
        ::execl("/sbin/reboot", "reboot", static_cast<char*>(nullptr));
        ::_exit(1);
    }
    NC_LOGI("[OTA] 已调度延迟重启 child_pid={}", pid);
}

} // namespace ota
} // namespace iot_agent
