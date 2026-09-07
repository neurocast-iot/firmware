// nc::common 文件工具实现（最小可用版）
// TODO(第2步): 用 ota_agent/iot_monitor 现有实现替换/合并
#include "nc/common/file_utils.h"

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace nc {
namespace common {

bool FileExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool ReadFile(const std::string& path, std::string& content) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    content = ss.str();
    return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f << content;
        if (!f.good()) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

int64_t FileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<int64_t>(st.st_size);
}

bool MakeDirs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/" || cur.empty()) {
                continue;
            }
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

bool CopyFile(const std::string& srcPath, const std::string& dstPath) {
    int fdSrc = ::open(srcPath.c_str(), O_RDONLY);
    if (fdSrc < 0) {
        return false;
    }
    int fdDst = ::open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdDst < 0) {
        ::close(fdSrc);
        return false;
    }

    char buf[4096];
    ssize_t nRead;
    bool ok = true;
    while ((nRead = ::read(fdSrc, buf, sizeof(buf))) > 0) {
        ssize_t nWritten = 0;
        while (nWritten < nRead) {
            ssize_t w = ::write(fdDst, buf + nWritten, nRead - nWritten);
            if (w < 0) {
                ok = false;
                break;
            }
            nWritten += w;
        }
        if (!ok) break;
    }
    if (nRead < 0) {
        ok = false;
    }

    ::close(fdSrc);
    ::close(fdDst);

    if (!ok) {
        ::unlink(dstPath.c_str());
    }
    return ok;
}

std::string ExtractFileName(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
}

std::string ThumbPathFromSource(const std::string& sourcePath) {
    if (sourcePath.empty()) {
        return "";
    }
    size_t dotPos = sourcePath.rfind('.');
    if (dotPos != std::string::npos) {
        return sourcePath.substr(0, dotPos) + "_thumb.jpg";
    }
    return sourcePath + "_thumb.jpg";
}

} // namespace common
} // namespace nc
