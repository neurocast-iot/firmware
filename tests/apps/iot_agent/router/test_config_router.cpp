/**
 * @file test_config_router.cpp
 * @brief 配置管理器单测
 *
 * 测什么：
 *   - 初始化（文件存在/不存在/非法 JSON）
 *   - handleAttributes 变更检测（新字段/相同值/值变了/多字段）
 *   - OTA 字段跳过
 *   - deleted 字段跳过
 *   - 配置持久化（保存后重新加载）
 */
#include "router/config_router.h"

#include "nc/common/file_utils.h"

#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

namespace iot_agent {
namespace {

/* 生成唯一临时文件路径 */
std::string tempPath() {
    char tmpl[] = "/tmp/config_router_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    return tmpl;
}

/* 写内容到文件 */
void writeFile(const std::string& path, const std::string& content) {
    nc::common::WriteFileAtomic(path, content);
}

/* 清理临时文件 */
void cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

/* 检查 JSON 字符串里是否包含指定 key */
bool jsonHasKey(const std::string& json, const std::string& key) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    cJSON* item = cJSON_GetObjectItem(root, key.c_str());
    bool found = (item != nullptr);
    cJSON_Delete(root);
    return found;
}

/* 获取 JSON 字符串里指定 key 的数值 */
int jsonGetInt(const std::string& json, const std::string& key) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return -1;
    cJSON* item = cJSON_GetObjectItem(root, key.c_str());
    int val = (item && cJSON_IsNumber(item)) ? static_cast<int>(item->valuedouble) : -1;
    cJSON_Delete(root);
    return val;
}

/* ---- 测试用例 ---- */

class ConfigRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_path = tempPath();
    }
    void TearDown() override {
        cleanup(m_path);
    }
    std::string m_path;
};

TEST_F(ConfigRouterTest, InitFileNotExists) {
    /* 文件不存在时 initialize 返回 true，getConfigJson 返回 {} */
    ConfigRouter router;
    EXPECT_TRUE(router.initialize(m_path));
    EXPECT_EQ(router.getConfigJson(), "{}");
}

TEST_F(ConfigRouterTest, InitFileExists) {
    /* 文件存在时加载成功 */
    writeFile(m_path, R"({"key1": "value1", "key2": 42})");

    ConfigRouter router;
    EXPECT_TRUE(router.initialize(m_path));

    std::string config = router.getConfigJson();
    EXPECT_TRUE(jsonHasKey(config, "key1"));
    EXPECT_EQ(jsonGetInt(config, "key2"), 42);
}

TEST_F(ConfigRouterTest, HandleAttributesNewField) {
    /* 新字段 → delta 非空，包含新字段 */
    writeFile(m_path, "{}");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes(R"({"new_field": "hello"})");
    EXPECT_FALSE(delta.empty());
    EXPECT_TRUE(jsonHasKey(delta, "new_field"));

    /* 配置里也应该有了 */
    EXPECT_TRUE(jsonHasKey(router.getConfigJson(), "new_field"));
}

TEST_F(ConfigRouterTest, HandleAttributesSameValue) {
    /* 相同值 → delta 为空（不触发下发） */
    writeFile(m_path, R"({"resolution": 1080})");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes(R"({"resolution": 1080})");
    EXPECT_TRUE(delta.empty());
}

TEST_F(ConfigRouterTest, HandleAttributesChangedValue) {
    /* 值变了 → delta 包含变更字段 */
    writeFile(m_path, R"({"resolution": 1080})");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes(R"({"resolution": 720})");
    EXPECT_FALSE(delta.empty());
    EXPECT_EQ(jsonGetInt(delta, "resolution"), 720);

    /* 配置里也更新了 */
    EXPECT_EQ(jsonGetInt(router.getConfigJson(), "resolution"), 720);
}

TEST_F(ConfigRouterTest, HandleAttributesMultipleChanges) {
    /* 多个字段同时变 → delta 包含所有变更 */
    writeFile(m_path, R"({"width": 1920, "height": 1080})");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes(R"({"width": 1280, "height": 720, "fps": 30})");
    EXPECT_FALSE(delta.empty());
    EXPECT_EQ(jsonGetInt(delta, "width"), 1280);
    EXPECT_EQ(jsonGetInt(delta, "height"), 720);
    EXPECT_EQ(jsonGetInt(delta, "fps"), 30);
}

TEST_F(ConfigRouterTest, OtaFieldsSkipped) {
    /* fw_version / sw_version 不进入配置（被 isOtaKey 跳过） */
    writeFile(m_path, "{}");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes(R"({
        "fw_version": "1.0.0",
        "sw_version": "2.0.0",
        "resolution": 1080
    })");

    /* delta 里只有 resolution，没有 OTA 字段 */
    EXPECT_TRUE(jsonHasKey(delta, "resolution"));
    EXPECT_FALSE(jsonHasKey(delta, "fw_version"));
    EXPECT_FALSE(jsonHasKey(delta, "sw_version"));

    /* 配置里也没有 OTA 字段 */
    EXPECT_FALSE(jsonHasKey(router.getConfigJson(), "fw_version"));
    EXPECT_FALSE(jsonHasKey(router.getConfigJson(), "sw_version"));
}

TEST_F(ConfigRouterTest, DeletedFieldSkipped) {
    /* deleted 字段不进入配置 */
    writeFile(m_path, "{}");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes(R"({"deleted": "some_key"})");
    EXPECT_TRUE(delta.empty());
    EXPECT_FALSE(jsonHasKey(router.getConfigJson(), "deleted"));
}

TEST_F(ConfigRouterTest, SaveAndReload) {
    /* handleAttributes 后，新建一个 ConfigRouter 重新加载，配置还在 */
    writeFile(m_path, "{}");

    {
        ConfigRouter router;
        ASSERT_TRUE(router.initialize(m_path));
        router.handleAttributes(R"({"resolution": 1080, "fps": 30})");
    }

    /* 重新加载 */
    ConfigRouter router2;
    ASSERT_TRUE(router2.initialize(m_path));

    std::string config = router2.getConfigJson();
    EXPECT_EQ(jsonGetInt(config, "resolution"), 1080);
    EXPECT_EQ(jsonGetInt(config, "fps"), 30);
}

TEST_F(ConfigRouterTest, InitInvalidJsonFile) {
    /* 文件内容是非法 JSON → initialize 成功但配置为空 */
    writeFile(m_path, "not valid json {{{");

    ConfigRouter router;
    /* initialize 总是返回 true（文件不存在也算成功） */
    EXPECT_TRUE(router.initialize(m_path));
    /* 但配置为空 */
    EXPECT_EQ(router.getConfigJson(), "{}");
}

TEST_F(ConfigRouterTest, HandleAttributesInvalidJson) {
    /* handleAttributes 收到非法 JSON → 返回空 delta */
    writeFile(m_path, "{}");

    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));

    std::string delta = router.handleAttributes("invalid json {{{");
    EXPECT_TRUE(delta.empty());
}

TEST_F(ConfigRouterTest, ConfigPathAccessor) {
    /* configPath() 返回初始化时传入的路径 */
    ConfigRouter router;
    ASSERT_TRUE(router.initialize(m_path));
    EXPECT_EQ(router.configPath(), m_path);
}

} // namespace
} // namespace iot_agent
