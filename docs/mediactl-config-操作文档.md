# mediactl config 操作文档

`mediactl` 是 mediad 的板上调试 CLI（curl 之于 http 服务），直连 mediad 的 IPC 命令通道，
不经 MQTT/iot_live，用于单独验证 mediad 本身是否正常。

```
mediactl [-e <cmd-endpoint>] config get                # 查询当前生效的全量配置
mediactl [-e <cmd-endpoint>] config set '<json-patch>' # 下发局部配置（增量合并）
```

默认端点 `ipc:///tmp/nc_mediad_cmd.ipc`，与 mediad 同机执行时无需 `-e`。

---

## 1. 获取当前生效的配置

```bash
mediactl config get
```

应答的 `data` 字段就是当前内存中生效的全量配置（与落盘的
`/etc/config/mediad.json` 同源）：

```json
{
    "code": 0,
    "message": "ok",
    "data": {
        "device_id": "A4C1380092CFA43E-C",
        "camera": {
            "auto_start": true,
            "main": { "width": 1920, "height": 1080, "fps": 25, "bitrate_kbps": 2048 },
            "sub":  { "width": 640,  "height": 480,  "fps": 15, "bitrate_kbps": 512 }
        },
        "...": "..."
    }
}
```

排查时先 `config get` 确认基线，改完再 `config get` 验证已生效。

## 2. 设置参数（增量合并）

`config set` 的 JSON 是**补丁**语义：只需携带要改的键，未出现的键保持当前值。
合并成功后 mediad 立即生效并原子回写配置文件（断电不丢）。

```bash
# 只改一个值
mediactl config set '{"snapshot":{"quality":90}}'

# 同时改多个段
mediactl config set '{"device_id":"A4C1380092CFA43E-C","snapshot":{"scheduled":{"enabled":true,"interval_sec":60}}}'
```

成功应答 `{"code":0,"message":"ok"}`；JSON 非法或键类型不对返回 `code:-1` 及原因。

> 数值有服务端钳位兜底：`quality` 1-100、`interval_sec`>=1、`segment_sec`>=5、
> `relay_idle_sec`>=15 等，下发离谱值会被拉回合法范围而不是报错。

## 3. 相机参数排查流程

### 3.1 可调参数

| 键 | 说明 | 生效方式 |
|---|---|---|
| `camera.main.width` / `height` | 主通道分辨率 | **重启相机**（VI 重建，秒级黑屏） |
| `camera.main.fps` | 主通道帧率 | 热更（编码器动态调整，不断流） |
| `camera.main.bitrate_kbps` | 主通道码率 | 热更 |
| `camera.sub.*` | 子通道（快照用）同上 | 同上 |

分辨率变化会触发完整的重启编排：直播/录像/拍照先暂停 → 相机以新分辨率
重建 → 各服务自动恢复。fps/码率只走编码器热更路径。

### 3.2 操作步骤

```bash
# ① 看基线
mediactl config get

# ② 降码率（热更，最常用的带宽调试手段）
mediactl config set '{"camera":{"main":{"bitrate_kbps":1024}}}'

# ③ 降帧率（热更）
mediactl config set '{"camera":{"main":{"fps":15}}}'

# ④ 换分辨率（触发相机重启编排）
mediactl config set '{"camera":{"main":{"width":1280,"height":720}}}'

# ⑤ 确认新配置已生效并落盘
mediactl config get
```

### 3.3 判断 mediad 是否正常

**看应答**：每条 `config set` 应答里 `code:0` 即 mediad 收到并合并成功。

**看事件**（另开一个终端）：

```bash
mediactl listen
# 期望输出：
# [event 12102] {"restarted":false}   ← 热更路径（fps/码率）
# [event 12102] {"restarted":true}    ← 重启路径（分辨率）
# [event 12101] {"camera":"running"}  ← 相机重启完成
```

**看日志**（mediad 前台运行时）：

```
热更：  [APPLY] Hot-swap channel 0: fps=25->15, bitrate=2048->1024
重启：  [APPLY] Restarting channel 0: resolution 1920x1080 -> 1280x720
跳过：  [APPLY] Channel 0 not exist, skip            ← 见下方注意事项 ①
        [APPLY] Channel 1 has no encoder (snapshot channel), skip hot-swap  ← 正常
```

**看服务状态**：

```bash
mediactl status
# data: {"camera":"running","live_mode":"idle","live_viewers":0,...}
```

### 3.4 注意事项

① **主通道按订阅动态启停**：无人观看/不在录像时通道 0 未创建，此时改 fps/码率
日志会显示 `Channel 0 not exist, skip`——配置已保存，等观看者接入、通道创建时
自然按新值生效。想立即验证热更效果，先开一路 P2P 观看或 `record start` 再下发。

② **传感器帧率上限**：GC4653 高光模式 ISP 钳在 20fps，`fps` 配 25 实际出帧约 20，
属传感器限制不是故障（推流统计 `total/时长≈20` 即正常）。

③ **通道 1 无编码器**：快照通道只开 VI，拍照时临时建 VENC，热更日志中
`Channel 1 has no encoder ... skip` 是预期行为。

## 4. 其他常用配置示例

```bash
# 定时拍照：每 60 秒一张
mediactl config set '{"snapshot":{"scheduled":{"enabled":true,"interval_sec":60}}}'

# 关闭定时拍照
mediactl config set '{"snapshot":{"scheduled":{"enabled":false}}}'

# 下发设备 ID（正式环境由 IoT 进程下发，调试可手动）
mediactl config set '{"device_id":"A4C1380092CFA43E-C"}'

# 关闭/开启实时视频服务（触发 LiveStreamService 整体重启）
mediactl config set '{"live":{"enabled":false}}'

# 调整 P2P 协商超时与 SRS 空闲停推窗口
mediactl config set '{"live":{"p2p_timeout_sec":10,"relay_idle_sec":20}}'
```

## 5. 故障速查

| 现象 | 原因 | 处理 |
|---|---|---|
| `error: connect ... failed (mediad not running?)` | mediad 没起来或 cmd 端点不通 | `ps \| grep mediad`；核对 `-e` 端点 |
| `error: request failed: 接收响应超时` | mediad 卡死或处理中崩溃 | 看 mediad 日志最后几行定位崩溃点 |
| 应答 `code:-1 invalid json` | patch JSON 语法错误 | 检查引号/括号（shell 内层用双引号） |
| `config set` 成功但行为没变 | 改的通道当前未启用（见 3.4 ①） | 先拉起观看/录像再验证 |
