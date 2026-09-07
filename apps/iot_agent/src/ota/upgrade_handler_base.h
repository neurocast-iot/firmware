/**
 * @file upgrade_handler_base.h
 * @brief 文件级升级处理器基类
 *
 * 定义共同流程：解压 → 解析 manifest → 备份 → 安装 → 校验 → 失败回滚 → 清理。
 * sw 和 fw 都继承这个基类，只需要实现差异化部分（备份目录、日志前缀）。
 *
 * 未来如果 fw 要改成 A/B 分区烧写，只需要在 FwHandler 里覆盖 executeUpgrade()，
 * 基类和 BackupManager 不动。
 */
#pragma once

#include "backup_manager.h"
#include "ota_config.h"
#include "ota_types.h"

#include <memory>
#include <string>

namespace iot_agent {
namespace ota {

class UpgradeHandlerBase {
public:
    virtual ~UpgradeHandlerBase() = default;

    /**
     * @brief 执行完整升级（模板方法，定义共同流程）
     * @param package_path 升级包路径（.tar.gz）
     * @param staging_dir 解压临时目录
     * @param out_version 输出的实际版本号（从 manifest 读）
     * @param out_error 失败时的错误描述
     * @return true=升级成功
     *
     * 流程：解压 → 解析 manifest → 备份 → 安装 → 校验 → 失败回滚 → 清理。
     * 子类一般不需要覆盖这个方法，除非要改成完全不同的升级方式（比如 A/B 分区烧写）。
     */
    virtual bool executeUpgrade(const std::string& package_path,
                                const std::string& staging_dir,
                                std::string& out_version,
                                std::string& out_error);

protected:
    /** 子类实现：返回备份根目录（比如 OtaPaths::SW_BACKUP 或 OtaPaths::FW_BACKUP） */
    virtual std::string getBackupRoot() const = 0;

    /** 子类实现：返回日志前缀（比如 "[SwHandler]" 或 "[FwHandler]"） */
    virtual const char* getLogPrefix() const = 0;

    /** 解压升级包到 staging 目录 */
    bool extractPackage(const std::string& package_path,
                        const std::string& staging_dir,
                        std::string& out_error);

    /** 把 manifest 里每个文件从 staging 拷到目标路径 */
    virtual bool installFiles(const SwManifest& manifest,
                              const std::string& staging_dir,
                              std::string& out_error);

    /** 安装后逐个算 SHA256 和 manifest 里声明的比对 */
    bool verifyNewFiles(const SwManifest& manifest, std::string& out_error);

    /** 从备份回滚 */
    bool rollback(const std::string& version, std::string& out_error);

    /** 清理临时文件 */
    void cleanup(const std::string& staging_dir, const std::string& package_path);

    /** 备份管理器（延迟初始化，第一次调用 getBackupManager() 时创建） */
    BackupManager& getBackupManager();

private:
    std::unique_ptr<BackupManager> m_backupManager;
};

} // namespace ota
} // namespace iot_agent
