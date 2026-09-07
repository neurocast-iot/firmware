/**
 * @file test_config_store.cpp
 * @brief mediad ConfigStore 单测
 *
 * 测什么：
 *   - 各配置段的阈值范围校验（ChannelCfg/CameraCfg/SnapshotCfg/ThumbnailCfg/RecordCfg）
 *   - 边界值测试（刚好在边界、刚好超出边界）
 *   - 配置加载/合并/校验/拒绝流程
 *   - OSD 规范化（千分比钳位、字号回落、坏元素剔除）
 *   - osdCfgChanged 变更检测
 *   - toChannelCfg / toCameraConfig 转换
 *   - LiveCfg::whipUrl URL 模板替换
 *   - ConfigStore 的 load/applyUpdate/get/addListener
 */
#include "config/ConfigStore.h"

#include "nc/common/file_utils.h"

#include <cstdio>
#include <string>

#include <gtest/gtest.h>

namespace mediad {
namespace {

/* ---- 工具函数 ---- */

std::string tempPath() {
    char tmpl[] = "/tmp/config_store_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    return tmpl;
}

void writeFile(const std::string& path, const std::string& content) {
    nc::common::WriteFileAtomic(path, content);
}

void cleanup(const std::string& path) {
    std::remove(path.c_str());
}

/* =========================================================================
 * ChannelCfg::check() 阈值范围测试
 *
 * 规则：
 *   - 宽 [160, 2560]，必须是 32 的倍数
 *   - 高 [128, 1440]，必须是 8 的倍数
 *   - fps [1, 30]
 *   - bitrateKbps [64, 16384]
 * ========================================================================= */

class ChannelCfgCheckTest : public ::testing::Test {
protected:
    ChannelCfg cfg;
    std::string errMsg;
};

/* 分辨率边界测试 */
TEST_F(ChannelCfgCheckTest, WidthBelowMin_Rejected) {
    cfg.width = 159; cfg.height = 128; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_FALSE(cfg.check("test", errMsg));
    EXPECT_NE(errMsg.find("resolution"), std::string::npos);
}

TEST_F(ChannelCfgCheckTest, WidthAtMin_Accepted) {
    cfg.width = 160; cfg.height = 128; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, WidthAboveMax_Rejected) {
    cfg.width = 2561; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_FALSE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, WidthAtMax_Accepted) {
    cfg.width = 2560; cfg.height = 1440; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, WidthNotMultipleOf32_Rejected) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));  /* 1920 = 60*32, OK */

    cfg.width = 1900;  /* 1900 / 32 = 59.375, 不是 32 的倍数 */
    EXPECT_FALSE(cfg.check("test", errMsg));
    EXPECT_NE(errMsg.find("multiple of 32"), std::string::npos);
}

TEST_F(ChannelCfgCheckTest, HeightBelowMin_Rejected) {
    cfg.width = 160; cfg.height = 127; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_FALSE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, HeightAtMin_Accepted) {
    cfg.width = 160; cfg.height = 128; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, HeightAboveMax_Rejected) {
    cfg.width = 1920; cfg.height = 1441; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_FALSE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, HeightAtMax_Accepted) {
    cfg.width = 1920; cfg.height = 1440; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, HeightNotMultipleOf8_Rejected) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));  /* 1080 = 135*8, OK */

    cfg.height = 1079;  /* 1079 / 8 = 134.875, 不是 8 的倍数 */
    EXPECT_FALSE(cfg.check("test", errMsg));
    EXPECT_NE(errMsg.find("multiple of 8"), std::string::npos);
}

/* FPS 边界测试 */
TEST_F(ChannelCfgCheckTest, FpsBelowMin_Rejected) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 0; cfg.bitrateKbps = 1024;
    EXPECT_FALSE(cfg.check("test", errMsg));
    EXPECT_NE(errMsg.find("fps"), std::string::npos);
}

TEST_F(ChannelCfgCheckTest, FpsAtMin_Accepted) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 1; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, FpsAtMax_Accepted) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 30; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, FpsAboveMax_Rejected) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 31; cfg.bitrateKbps = 1024;
    EXPECT_FALSE(cfg.check("test", errMsg));
}

/* 码率边界测试 */
TEST_F(ChannelCfgCheckTest, BitrateBelowMin_Rejected) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 63;
    EXPECT_FALSE(cfg.check("test", errMsg));
    EXPECT_NE(errMsg.find("bitrate_kbps"), std::string::npos);
}

TEST_F(ChannelCfgCheckTest, BitrateAtMin_Accepted) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 64;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, BitrateAtMax_Accepted) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 16384;
    EXPECT_TRUE(cfg.check("test", errMsg));
}

TEST_F(ChannelCfgCheckTest, BitrateAboveMax_Rejected) {
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 16385;
    EXPECT_FALSE(cfg.check("test", errMsg));
}

/* 典型分辨率测试 */
TEST_F(ChannelCfgCheckTest, TypicalResolutions_Accepted) {
    /* 1080p */
    cfg.width = 1920; cfg.height = 1080; cfg.fps = 25; cfg.bitrateKbps = 2048;
    EXPECT_TRUE(cfg.check("main", errMsg));

    /* 720p */
    cfg.width = 1280; cfg.height = 720; cfg.fps = 25; cfg.bitrateKbps = 1024;
    EXPECT_TRUE(cfg.check("main", errMsg));

    /* VGA */
    cfg.width = 640; cfg.height = 480; cfg.fps = 15; cfg.bitrateKbps = 512;
    EXPECT_TRUE(cfg.check("sub", errMsg));

    /* QVGA */
    cfg.width = 320; cfg.height = 240; cfg.fps = 15; cfg.bitrateKbps = 256;
    EXPECT_TRUE(cfg.check("sub", errMsg));

    /* 最小可用分辨率 */
    cfg.width = 160; cfg.height = 128; cfg.fps = 1; cfg.bitrateKbps = 64;
    EXPECT_TRUE(cfg.check("sub", errMsg));
}

/* =========================================================================
 * CameraCfg::check() 测试
 * ========================================================================= */

TEST(CameraCfgCheckTest, BothChannelsValid_Accepted) {
    CameraCfg cfg;
    cfg.main = {1920, 1080, 25, 2048};
    cfg.sub = {640, 480, 15, 512};
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(CameraCfgCheckTest, MainChannelInvalid_Rejected) {
    CameraCfg cfg;
    cfg.main.width = 100;  /* 低于下限 160 */
    cfg.sub = {640, 480, 15, 512};
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
    EXPECT_NE(errMsg.find("camera.main"), std::string::npos);
}

TEST(CameraCfgCheckTest, SubChannelInvalid_Rejected) {
    CameraCfg cfg;
    cfg.main = {1920, 1080, 25, 2048};
    cfg.sub.fps = 0;  /* 低于下限 1 */
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
    EXPECT_NE(errMsg.find("camera.sub"), std::string::npos);
}

/* =========================================================================
 * SnapshotCfg::check() 阈值测试
 *
 * 规则：quality [1, 100]
 * ========================================================================= */

TEST(SnapshotCfgCheckTest, QualityBelowMin_Rejected) {
    SnapshotCfg cfg;
    cfg.quality = 0;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

TEST(SnapshotCfgCheckTest, QualityAtMin_Accepted) {
    SnapshotCfg cfg;
    cfg.quality = 1;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(SnapshotCfgCheckTest, QualityAtMax_Accepted) {
    SnapshotCfg cfg;
    cfg.quality = 100;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(SnapshotCfgCheckTest, QualityAboveMax_Rejected) {
    SnapshotCfg cfg;
    cfg.quality = 101;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

/* =========================================================================
 * ThumbnailCfg::check() 阈值测试
 *
 * 规则：width [64, 640], height [64, 480], quality [1, 100]
 * ========================================================================= */

TEST(ThumbnailCfgCheckTest, WidthBelowMin_Rejected) {
    ThumbnailCfg cfg;
    cfg.width = 63; cfg.height = 176; cfg.quality = 60;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
    EXPECT_NE(errMsg.find("thumbnail.width"), std::string::npos);
}

TEST(ThumbnailCfgCheckTest, WidthAtMin_Accepted) {
    ThumbnailCfg cfg;
    cfg.width = 64; cfg.height = 176; cfg.quality = 60;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(ThumbnailCfgCheckTest, WidthAtMax_Accepted) {
    ThumbnailCfg cfg;
    cfg.width = 640; cfg.height = 176; cfg.quality = 60;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(ThumbnailCfgCheckTest, WidthAboveMax_Rejected) {
    ThumbnailCfg cfg;
    cfg.width = 641; cfg.height = 176; cfg.quality = 60;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

TEST(ThumbnailCfgCheckTest, HeightBelowMin_Rejected) {
    ThumbnailCfg cfg;
    cfg.width = 320; cfg.height = 63; cfg.quality = 60;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

TEST(ThumbnailCfgCheckTest, HeightAtMin_Accepted) {
    ThumbnailCfg cfg;
    cfg.width = 320; cfg.height = 64; cfg.quality = 60;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(ThumbnailCfgCheckTest, HeightAtMax_Accepted) {
    ThumbnailCfg cfg;
    cfg.width = 320; cfg.height = 480; cfg.quality = 60;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(ThumbnailCfgCheckTest, HeightAboveMax_Rejected) {
    ThumbnailCfg cfg;
    cfg.width = 320; cfg.height = 481; cfg.quality = 60;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

/* =========================================================================
 * RecordCfg::check() 阈值测试
 *
 * 规则：segmentSec [5, 3600]
 * ========================================================================= */

TEST(RecordCfgCheckTest, SegmentSecBelowMin_Rejected) {
    RecordCfg cfg;
    cfg.segmentSec = 4;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
    EXPECT_NE(errMsg.find("record.segment_sec"), std::string::npos);
}

TEST(RecordCfgCheckTest, SegmentSecAtMin_Accepted) {
    RecordCfg cfg;
    cfg.segmentSec = 5;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(RecordCfgCheckTest, SegmentSecAtMax_Accepted) {
    RecordCfg cfg;
    cfg.segmentSec = 3600;
    std::string errMsg;
    EXPECT_TRUE(cfg.check(errMsg));
}

TEST(RecordCfgCheckTest, SegmentSecAboveMax_Rejected) {
    RecordCfg cfg;
    cfg.segmentSec = 3601;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

TEST(RecordCfgCheckTest, SegmentSecZero_Rejected) {
    RecordCfg cfg;
    cfg.segmentSec = 0;  /* 0 会变成永不分段，写爆磁盘 */
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

/* =========================================================================
 * MediadConfig::check() 综合校验测试
 * ========================================================================= */

TEST(MediadConfigCheckTest, AllValid_Accepted) {
    MediadConfig cfg;
    EXPECT_TRUE(cfg.check(/* errMsg */ *(new std::string())));
}

TEST(MediadConfigCheckTest, CameraInvalid_Rejected) {
    MediadConfig cfg;
    cfg.camera.main.fps = 0;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

TEST(MediadConfigCheckTest, SnapshotInvalid_Rejected) {
    MediadConfig cfg;
    cfg.snapshot.quality = 200;
    std::string errMsg;
    EXPECT_FALSE(cfg.check(errMsg));
}

/* =========================================================================
 * toChannelCfg / toCameraConfig 转换测试
 * ========================================================================= */

TEST(ConversionTest, ToChannelCfg_CopiesFields) {
    camera::ChannelConfig src;
    src.width = 1920;
    src.height = 1080;
    src.fps = 25;
    src.bitrate = 2048;

    ChannelCfg dst = toChannelCfg(src);
    EXPECT_EQ(dst.width, 1920);
    EXPECT_EQ(dst.height, 1080);
    EXPECT_EQ(dst.fps, 25);
    EXPECT_EQ(dst.bitrateKbps, 2048);
}

TEST(ConversionTest, ToCameraConfig_CopiesFields) {
    CameraCfg src;
    src.sensorConfig = "/test/sensor.conf";
    src.main = {1920, 1080, 25, 2048, "h265", "cbr"};
    src.sub = {640, 480, 15, 512, "h264", "vbr"};

    camera::CameraConfig dst = toCameraConfig(src);
    EXPECT_EQ(dst.sensorConfig(), "/test/sensor.conf");
    EXPECT_EQ(dst.mainChannel().width, 1920);
    EXPECT_EQ(dst.mainChannel().height, 1080);
    EXPECT_EQ(dst.mainChannel().fps, 25);
    EXPECT_EQ(dst.mainChannel().bitrate, 2048);
    EXPECT_EQ(static_cast<int>(dst.mainChannel().codec), static_cast<int>(camera::CodecType::H265));
    EXPECT_EQ(static_cast<int>(dst.mainChannel().brMode), static_cast<int>(camera::BitrateMode::CBR));
    EXPECT_EQ(dst.subChannel().width, 640);
}

/* =========================================================================
 * LiveCfg::whipUrl() URL 模板替换测试
 * ========================================================================= */

TEST(LiveCfgWhipUrlTest, EmptyDeviceId_ReturnsEmpty) {
    LiveCfg cfg;
    EXPECT_TRUE(cfg.whipUrl("", "token123").empty());
}

TEST(LiveCfgWhipUrlTest, ReplacesDeviceUid) {
    LiveCfg cfg;
    cfg.srs.whipUrlTemplate = "http://srs/live/{deviceUid}";
    EXPECT_EQ(cfg.whipUrl("dev001"), "http://srs/live/dev001");
}

TEST(LiveCfgWhipUrlTest, ReplacesAccessToken) {
    LiveCfg cfg;
    cfg.srs.whipUrlTemplate = "http://srs/live/{deviceUid}?token={accessToken}";
    EXPECT_EQ(cfg.whipUrl("dev001", "abc123"), "http://srs/live/dev001?token=abc123");
}

TEST(LiveCfgWhipUrlTest, EmptyAccessToken_RemovesPlaceholder) {
    LiveCfg cfg;
    cfg.srs.whipUrlTemplate = "http://srs/live/{deviceUid}?token={accessToken}";
    EXPECT_EQ(cfg.whipUrl("dev001", ""), "http://srs/live/dev001?token=");
}

TEST(LiveCfgWhipUrlTest, NoPlaceholders_ReturnsTemplateAsIs) {
    LiveCfg cfg;
    cfg.srs.whipUrlTemplate = "http://srs/live/stream1";
    EXPECT_EQ(cfg.whipUrl("dev001"), "http://srs/live/stream1");
}

/* =========================================================================
 * osdCfgChanged() 变更检测测试
 * ========================================================================= */

TEST(OsdCfgChangedTest, SameConfig_ReturnsFalse) {
    OsdCfg a, b;
    a.enabled = true;
    a.fontFile = "/fonts/test.bin";
    b = a;
    EXPECT_FALSE(osdCfgChanged(a, b));
}

TEST(OsdCfgChangedTest, EnabledChanged_ReturnsTrue) {
    OsdCfg a, b;
    a.enabled = true;
    b.enabled = false;
    EXPECT_TRUE(osdCfgChanged(a, b));
}

TEST(OsdCfgChangedTest, FontFileChanged_ReturnsTrue) {
    OsdCfg a, b;
    a.fontFile = "/fonts/a.bin";
    b.fontFile = "/fonts/b.bin";
    EXPECT_TRUE(osdCfgChanged(a, b));
}

TEST(OsdCfgChangedTest, ElementsSizeChanged_ReturnsTrue) {
    OsdCfg a, b;
    a.elements.push_back({});
    EXPECT_TRUE(osdCfgChanged(a, b));
}

TEST(OsdCfgChangedTest, ElementFieldChanged_ReturnsTrue) {
    OsdCfg a, b;
    OsdCfg::ElemCfg e1, e2;
    e1.id = "elem1";
    e2.id = "elem2";
    a.elements.push_back(e1);
    b.elements.push_back(e2);
    EXPECT_TRUE(osdCfgChanged(a, b));
}

/* =========================================================================
 * ConfigStore 加载/更新测试
 * ========================================================================= */

class ConfigStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = tempPath();
    }
    void TearDown() override {
        cleanup(path);
    }
    std::string path;
};

TEST_F(ConfigStoreTest, LoadNonExistentFile_ReturnsFalse) {
    ConfigStore store;
    EXPECT_FALSE(store.load("/tmp/nonexistent_config_xyz.json"));
    /* 加载失败后用默认值 */
    MediadConfig cfg = store.get();
    EXPECT_EQ(cfg.camera.main.width, 1920);
}

TEST_F(ConfigStoreTest, LoadInvalidJson_ReturnsFalse) {
    writeFile(path, "not valid json {{{");
    ConfigStore store;
    EXPECT_FALSE(store.load(path));
}

TEST_F(ConfigStoreTest, LoadValidJson_ReturnsTrue) {
    writeFile(path, R"({"device_id": "test_dev_001"})");
    ConfigStore store;
    EXPECT_TRUE(store.load(path));
    EXPECT_EQ(store.get().deviceId, "test_dev_001");
}

TEST_F(ConfigStoreTest, LoadPartialJson_KeepsDefaults) {
    writeFile(path, R"({"camera": {"main": {"width": 1280}}})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    MediadConfig cfg = store.get();
    EXPECT_EQ(cfg.camera.main.width, 1280);
    EXPECT_EQ(cfg.camera.main.height, 1080);  /* 默认值保留 */
}

TEST_F(ConfigStoreTest, ApplyUpdate_ValidPatch_Succeeds) {
    writeFile(path, "{}");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));

    std::string errMsg;
    EXPECT_TRUE(store.applyUpdate(R"({"device_id": "new_dev"})", errMsg));
    EXPECT_EQ(store.get().deviceId, "new_dev");
}

TEST_F(ConfigStoreTest, ApplyUpdate_InvalidConfig_Rejected) {
    writeFile(path, "{}");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));

    std::string errMsg;
    /* fps = 0 不合法，应该被拒绝 */
    EXPECT_FALSE(store.applyUpdate(R"({"camera": {"main": {"fps": 0}}})", errMsg));
    EXPECT_NE(errMsg.find("fps"), std::string::npos);
    /* 原配置不变 */
    EXPECT_EQ(store.get().camera.main.fps, 25);
}

TEST_F(ConfigStoreTest, ApplyUpdate_InvalidJson_Rejected) {
    writeFile(path, "{}");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));

    std::string errMsg;
    EXPECT_FALSE(store.applyUpdate("not json", errMsg));
    EXPECT_EQ(errMsg, "invalid json");
}

TEST_F(ConfigStoreTest, AddListener_CalledOnUpdate) {
    writeFile(path, R"({"device_id": "old"})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));

    bool called = false;
    std::string oldId, newId;
    store.addListener([&](const MediadConfig& oldCfg, const MediadConfig& newCfg) {
        called = true;
        oldId = oldCfg.deviceId;
        newId = newCfg.deviceId;
    });

    std::string errMsg;
    ASSERT_TRUE(store.applyUpdate(R"({"device_id": "new"})", errMsg));
    EXPECT_TRUE(called);
    EXPECT_EQ(oldId, "old");
    EXPECT_EQ(newId, "new");
}

/* =========================================================================
 * OSD 规范化测试（通过 ConfigStore::load 间接测试）
 * ========================================================================= */

TEST_F(ConfigStoreTest, OsdNormalization_ClampsPermille) {
    writeFile(path, R"({"osd": {"elements": [{"type": "label", "x": 1500, "y": -100}]}})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    OsdCfg cfg = store.get().osd;
    ASSERT_EQ(cfg.elements.size(), 1u);
    EXPECT_EQ(cfg.elements[0].x, 1000);  /* 钳位到上限 */
    EXPECT_EQ(cfg.elements[0].y, 0);     /* 钳位到下限 */
}

TEST_F(ConfigStoreTest, OsdNormalization_NormalizesSize) {
    writeFile(path, R"({"osd": {"elements": [{"type": "label", "size": "huge"}]}})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    OsdCfg cfg = store.get().osd;
    ASSERT_EQ(cfg.elements.size(), 1u);
    EXPECT_EQ(cfg.elements[0].size, "medium");  /* 未知档位回落 medium */
}

TEST_F(ConfigStoreTest, OsdNormalization_RemovesUnknownType) {
    writeFile(path, R"({"osd": {"elements": [{"type": "unknown"}, {"type": "label"}]}})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    OsdCfg cfg = store.get().osd;
    EXPECT_EQ(cfg.elements.size(), 1u);  /* unknown 类型被剔除 */
    EXPECT_EQ(cfg.elements[0].type, "label");
}

TEST_F(ConfigStoreTest, OsdNormalization_RemovesInvalidPolygon) {
    /* 多边形少于 3 个点会被剔除 */
    writeFile(path, R"({"osd": {"elements": [{"type": "polygon", "points": [[0,0], [100,100]]}]}})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    OsdCfg cfg = store.get().osd;
    EXPECT_EQ(cfg.elements.size(), 0u);  /* 只有 2 个点，被剔除 */
}

TEST_F(ConfigStoreTest, OsdNormalization_TruncatesPolygonPoints) {
    /* 多边形超过 16 个点会截断 */
    std::string json = R"({"osd": {"elements": [{"type": "polygon", "points": [)";
    for (int i = 0; i < 20; ++i) {
        if (i > 0) json += ",";
        json += "[" + std::to_string(i * 50) + "," + std::to_string(i * 50) + "]";
    }
    json += "]}]}}";
    writeFile(path, json);
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    OsdCfg cfg = store.get().osd;
    ASSERT_EQ(cfg.elements.size(), 1u);
    EXPECT_EQ(cfg.elements[0].points.size(), 16u);  /* 截断到 16 */
}

TEST_F(ConfigStoreTest, OsdNormalization_DefaultOpacity) {
    writeFile(path, R"({"osd": {"elements": [{"type": "rect"}, {"type": "label"}]}})");
    ConfigStore store;
    ASSERT_TRUE(store.load(path));
    OsdCfg cfg = store.get().osd;
    ASSERT_EQ(cfg.elements.size(), 2u);
    EXPECT_EQ(cfg.elements[0].opacity, 20);  /* rect 默认半透 */
    EXPECT_EQ(cfg.elements[1].opacity, 0);   /* label 默认不透明 */
}

} // namespace
} // namespace mediad
