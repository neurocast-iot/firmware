/**
 * @file test_cloud_config_translator.cpp
 * @brief 云端配置字段翻译器单测
 *
 * 测什么：
 *   - 单个/多个字段翻译是否正确
 *   - 没有映射的字段是否被跳过
 *   - triggers 字段是否直接透传
 *   - 非法 JSON 是否原样返回
 *   - 字符串格式的 JSON 数组是否自动解析
 *   - 嵌套路径是否正确创建
 */
#include "config/cloud_config_translator.h"

#include "cJSON.h"

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

namespace iot_agent {
namespace {

/* ---- 工具函数 ---- */

/* 解析 JSON 字符串，返回 cJSON 对象（调用方负责释放） */
cJSON* parseJson(const std::string& json) {
    return cJSON_Parse(json.c_str());
}

/* ---- 测试用例 ---- */

TEST(CloudConfigTranslatorTest, SingleFieldTranslation) {
    /* 单个字段：main_resolution_width → camera.main.width */
    std::string input = R"({"main_resolution_width": 1920})";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    cJSON* camera = cJSON_GetObjectItem(root, "camera");
    ASSERT_NE(camera, nullptr);
    cJSON* mainObj = cJSON_GetObjectItem(camera, "main");
    ASSERT_NE(mainObj, nullptr);
    cJSON* width = cJSON_GetObjectItem(mainObj, "width");
    ASSERT_NE(width, nullptr);
    EXPECT_EQ(static_cast<int>(width->valuedouble), 1920);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, MultipleFieldsTranslation) {
    /* 多个字段同时翻译 */
    std::string input = R"({
        "main_resolution_width": 1920,
        "main_resolution_height": 1080,
        "sub_resolution_width": 640,
        "sub_resolution_height": 480
    })";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    /* 检查 camera.main.width */
    cJSON* mainWidth = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "camera"), "main"), "width");
    ASSERT_NE(mainWidth, nullptr);
    EXPECT_EQ(static_cast<int>(mainWidth->valuedouble), 1920);

    /* 检查 camera.sub.height */
    cJSON* subHeight = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "camera"), "sub"), "height");
    ASSERT_NE(subHeight, nullptr);
    EXPECT_EQ(static_cast<int>(subHeight->valuedouble), 480);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, UnknownFieldSkipped) {
    /* 没有映射的字段不出现在结果里 */
    std::string input = R"({"unknown_field_xyz": 123})";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    /* 结果应该是空对象（没有映射的字段都被跳过） */
    EXPECT_EQ(root->child, nullptr);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, TriggersPassThrough) {
    /* triggers 字段直接透传，不翻译 */
    std::string input = R"({
        "triggers": [{"type": "timer", "interval": 60}],
        "main_resolution_width": 1920
    })";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    /* triggers 应该原样存在 */
    cJSON* triggers = cJSON_GetObjectItem(root, "triggers");
    ASSERT_NE(triggers, nullptr);
    EXPECT_TRUE(cJSON_IsArray(triggers));
    EXPECT_EQ(cJSON_GetArraySize(triggers), 1);

    /* 同时 main_resolution_width 也应该被翻译 */
    cJSON* width = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(root, "camera"), "main"), "width");
    ASSERT_NE(width, nullptr);
    EXPECT_EQ(static_cast<int>(width->valuedouble), 1920);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, InvalidJsonReturnsInput) {
    /* 非法 JSON 原样返回 */
    std::string input = "this is not json {{{";
    std::string result = translateCloudToMediad(input);
    EXPECT_EQ(result, input);
}

TEST(CloudConfigTranslatorTest, EmptyJsonObject) {
    /* 空 JSON 对象 → 空 JSON 对象 */
    std::string input = "{}";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->child, nullptr);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, StringEncodedArray) {
    /* osd_elements 是字符串格式的 JSON 数组时，自动解析成真正的数组 */
    std::string input = R"({"osd_elements": "[{\"type\":\"time\",\"x\":100}]"})";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    /* 检查 osd.elements 是真正的数组，不是字符串 */
    cJSON* osd = cJSON_GetObjectItem(root, "osd");
    ASSERT_NE(osd, nullptr);
    cJSON* elements = cJSON_GetObjectItem(osd, "elements");
    ASSERT_NE(elements, nullptr);
    EXPECT_TRUE(cJSON_IsArray(elements));
    EXPECT_EQ(cJSON_GetArraySize(elements), 1);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, NestedPathCreation) {
    /* 深层路径 camera.main.width 正确创建嵌套对象 */
    std::string input = R"({"main_codec": "h264"})";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    /* camera → main → codec = "h264" */
    cJSON* camera = cJSON_GetObjectItem(root, "camera");
    ASSERT_NE(camera, nullptr);
    EXPECT_TRUE(cJSON_IsObject(camera));

    cJSON* mainObj = cJSON_GetObjectItem(camera, "main");
    ASSERT_NE(mainObj, nullptr);
    EXPECT_TRUE(cJSON_IsObject(mainObj));

    cJSON* codec = cJSON_GetObjectItem(mainObj, "codec");
    ASSERT_NE(codec, nullptr);
    EXPECT_STREQ(codec->valuestring, "h264");

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, AllMainCameraFields) {
    /* 主通道 6 个字段全部翻译正确 */
    std::string input = R"({
        "main_resolution_width": 1920,
        "main_resolution_height": 1080,
        "main_frame_rate": 30,
        "main_bitrate": 4096,
        "main_codec": "h265",
        "main_br_mode": "cbr"
    })";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    cJSON* mainObj = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "camera"), "main");
    ASSERT_NE(mainObj, nullptr);

    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(mainObj, "width")->valuedouble), 1920);
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(mainObj, "height")->valuedouble), 1080);
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(mainObj, "fps")->valuedouble), 30);
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(mainObj, "bitrate_kbps")->valuedouble), 4096);
    EXPECT_STREQ(cJSON_GetObjectItem(mainObj, "codec")->valuestring, "h265");
    EXPECT_STREQ(cJSON_GetObjectItem(mainObj, "br_mode")->valuestring, "cbr");

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, AllSubCameraFields) {
    /* 子通道 6 个字段全部翻译正确 */
    std::string input = R"({
        "sub_resolution_width": 640,
        "sub_resolution_height": 480,
        "sub_codec": "jpeg",
        "sub_frame_rate": 15,
        "sub_bitrate": 1024,
        "sub_br_mode": "vbr"
    })";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    cJSON* subObj = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "camera"), "sub");
    ASSERT_NE(subObj, nullptr);

    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(subObj, "width")->valuedouble), 640);
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(subObj, "height")->valuedouble), 480);
    EXPECT_STREQ(cJSON_GetObjectItem(subObj, "codec")->valuestring, "jpeg");
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(subObj, "fps")->valuedouble), 15);
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(subObj, "bitrate_kbps")->valuedouble), 1024);
    EXPECT_STREQ(cJSON_GetObjectItem(subObj, "br_mode")->valuestring, "vbr");

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, LiveStreamFields) {
    /* 推流/ICE 相关字段翻译 */
    std::string input = R"({
        "push_stream_url": "whip://srs/{stream}",
        "ice_host": "1.2.3.4",
        "ice_port": 3478,
        "ice_username": "user",
        "ice_password": "pass"
    })";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    cJSON* live = cJSON_GetObjectItem(root, "live");
    ASSERT_NE(live, nullptr);

    /* SRS whip_url_template */
    cJSON* srs = cJSON_GetObjectItem(live, "srs");
    ASSERT_NE(srs, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItem(srs, "whip_url_template")->valuestring, "whip://srs/{stream}");

    /* ICE */
    cJSON* ice = cJSON_GetObjectItem(live, "ice");
    ASSERT_NE(ice, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItem(ice, "host")->valuestring, "1.2.3.4");
    EXPECT_EQ(static_cast<int>(cJSON_GetObjectItem(ice, "port")->valuedouble), 3478);
    EXPECT_STREQ(cJSON_GetObjectItem(ice, "username")->valuestring, "user");
    EXPECT_STREQ(cJSON_GetObjectItem(ice, "password")->valuestring, "pass");

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, OsdFields) {
    /* OSD enable + elements 翻译 */
    std::string input = R"({
        "osd_enable": true,
        "osd_elements": [{"type": "time", "x": 100}]
    })";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    cJSON* osd = cJSON_GetObjectItem(root, "osd");
    ASSERT_NE(osd, nullptr);

    cJSON* enabled = cJSON_GetObjectItem(osd, "enabled");
    ASSERT_NE(enabled, nullptr);
    EXPECT_TRUE(cJSON_IsTrue(enabled));

    cJSON* elements = cJSON_GetObjectItem(osd, "elements");
    ASSERT_NE(elements, nullptr);
    EXPECT_TRUE(cJSON_IsArray(elements));
    EXPECT_EQ(cJSON_GetArraySize(elements), 1);

    cJSON_Delete(root);
}

TEST(CloudConfigTranslatorTest, RecordField) {
    /* 录像分段时长翻译 */
    std::string input = R"({"record_segment_sec": 60})";
    std::string result = translateCloudToMediad(input);

    cJSON* root = parseJson(result);
    ASSERT_NE(root, nullptr);

    cJSON* record = cJSON_GetObjectItem(root, "record");
    ASSERT_NE(record, nullptr);
    cJSON* segmentSec = cJSON_GetObjectItem(record, "segment_sec");
    ASSERT_NE(segmentSec, nullptr);
    EXPECT_EQ(static_cast<int>(segmentSec->valuedouble), 60);

    cJSON_Delete(root);
}

} // namespace
} // namespace iot_agent
