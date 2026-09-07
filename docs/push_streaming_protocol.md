# 推流模式与信令协议

## 概述

设备支持三种推流模式（PushMode），前端通过 MQTT 信令或 RPC 指定模式，设备只做执行，不做自动降级决策。

---

## 推流模式（PushMode）

| 模式 | 含义 | ICE 策略 | 适用场景 |
|------|------|---------|---------|
| `P2P` | WebRTC 直连 | 全候选（host + srflx + relay） | 跨网络，NAT 穿透 |
| `P2P_Host` | WebRTC 直连（仅局域网） | 仅 host 候选 | 同局域网 / 设备有公网 IP |
| `SFU` | 服务器转发（SRS） | 走 WHIP 推到 SRS | 多人观看 / P2P 不通时兜底 |

### 模式触发方式

| 模式 | 触发方式 | 说明 |
|------|---------|------|
| P2P | offer 带 `"mode": "p2p"` 或不传 | 全候选，可跨网络 |
| P2P_Host | offer 带 `"mode": "p2p_host"` | 仅局域网直连 |
| SFU | 前端调用 RPC `startLiveStream` | 设备推流到 SRS |

---

## MQTT 信令协议

### Topic 规则

```
p2p/{deviceId}/offer      ← 观看端发，设备收
p2p/{deviceId}/answer     → 设备发，观看端收
p2p/{deviceId}/cand/{role} → ICE 候选（role = device / viewer）
p2p/{deviceId}/bye        → 会话结束通知
p2p/{deviceId}/relay      → SFU 观看者接纳通知（前端走 WHEP 拉流）
p2p/{deviceId}/alive      → 心跳保活
```

### 消息格式

所有消息均为 JSON，包含公共字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `sid` | string | 会话 ID（由观看端生成） |
| `ts` | number | 时间戳（毫秒） |

#### 1. offer（观看端 → 设备）

```json
{
  "sid": "session-abc-123",
  "ts": 1724659200000,
  "sdp": "v=0\r\no=- 123...",
  "mode": "p2p"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `sid` | string | 是 | 会话 ID |
| `ts` | number | 是 | 时间戳（毫秒） |
| `sdp` | string | 是 | SDP offer 内容 |
| `mode` | string | 否 | 推流模式：`"p2p"`（默认）或 `"p2p_host"`（仅局域网直连） |

设备收到 offer 后，根据 `mode` 字段决定行为：
- `"p2p"` 或不传：全候选模式（host + srflx + relay），可跨网络 NAT 穿透
- `"p2p_host"`：仅 host 候选，适用于同局域网 / 设备有公网 IP
- 如需 SFU 模式，观看端应走 RPC `startLiveStream`，不发 offer

#### 2. answer（设备 → 观看端）

```json
{
  "sid": "session-abc-123",
  "ts": 1724659200001,
  "sdp": "v=0\r\no=- 456..."
}
```

#### 3. candidate（双方互发）

```json
{
  "sid": "session-abc-123",
  "ts": 1724659200002,
  "candidate": "candidate:1 1 udp 2130706431 192.168.1.100 5000 typ host"
}
```

Topic 按角色区分：
- 设备发：`p2p/{deviceId}/cand/device`
- 观看端发：`p2p/{deviceId}/cand/viewer`

#### 4. bye（设备 → 观看端）

会话结束通知，携带失败原因（`reason` 字段），前端可据此决定是否切 SFU。

```json
{
  "sid": "session-abc-123",
  "ts": 1724659230000,
  "reason": "timeout"
}
```

| reason | 含义 | 前端建议动作 |
|--------|------|-------------|
| `timeout` | P2P 连接超时（15s 无帧） | RPC `startLiveStream` 切 SFU |
| `negotiate_failed` | SDP 协商失败 | RPC `startLiveStream` 切 SFU |
| `no_camera` | 相机不可用 | 提示用户检查设备 |
| `camera_restarting` | 相机正在重启 | 等待几秒后重试 |
| `p2p_occupied` | P2P 已有观看者，不支持多人 | 提示用户或等待 |
| `sfu_active` | 当前是 SFU 多人模式，不接受 P2P | 直接走 SFU 拉流 |
| `push_failed` | WHIP 推流到 SRS 失败 | 提示用户检查网络 |
| （空/无） | 正常结束（观看端主动 bye） | 无需动作 |

触发场景：
- P2P 超时 / 协商失败 / 相机不可用 / 相机重启
- P2P 已有观看者（第 2 人请求 P2P）
- SFU 模式下有人请求 P2P
- SFU 推流失败
- 观看端主动发 bye 结束

#### 5. relay（设备 → 观看端）

SFU 观看者接纳通知，前端收到后走 WHEP 拉流。

```json
{
  "sid": "session-abc-123",
  "ts": 1724659230000,
  "url": ""
}
```

`url` 字段预留，当前为空。WHEP 拉流地址由后端 API 下发，不在此消息中。

#### 6. alive（双方互发）

心跳保活，防止长连接断开。

```json
{
  "sid": "session-abc-123",
  "ts": 1724659200000
}
```

---

## RPC 接口

### startLiveStream

前端调用，触发设备推流到 SRS（SFU 模式）。

**请求参数**：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `accessToken` | string | 是 | SRS 认证令牌（平台下发） |

**响应**：

```json
{
  "success": true,
  "message": ""
}
```

失败时：

```json
{
  "success": false,
  "message": "whip push failed (srs unreachable?)"
}
```

### stopLiveStream

前端调用，停止设备推流。

**请求参数**：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `accessToken` | string | 是 | 必须与 startLiveStream 时的 token 一致 |

**响应**：

```json
{
  "success": true,
  "message": ""
}
```

token 不一致时：

```json
{
  "success": false,
  "message": "token mismatch, push belongs to another session"
}
```

---

## 状态机

设备不做自动模式切换，所有切换由前端控制。

```
         offer (P2P/P2P_Host)
    ┌───────────────────────────┐
    │                           ▼
┌───────┐                 ┌─────────┐
│ Idle  │                 │   P2P   │
│       │◄────────────────│         │
└───────┘   bye/timeout    └─────────┘
    │                           
    │ RPC startLiveStream       
    ▼                           
┌─────────┐                     
│   SFU   │                     
│         │                     
└─────────┘                     
    │                           
    │ 所有观看者离开              
    │ 或 RPC stopLiveStream     
    ▼                           
  Idle                          
```

**模式冲突处理**（设备不做编排，直接拒绝）：

| 当前状态 | 新请求 | 设备行为 |
|---------|--------|--------|
| P2P | P2P（第 2 人） | 拒绝，bye `p2p_occupied` |
| P2P | startLiveStream RPC | 停 P2P，起 SFU |
| SFU | P2P offer | 拒绝，bye `sfu_active` |
| SFU | startLiveStream（同 token） | 跳过，不重复推流 |
| SFU | startLiveStream（不同 token） | 停旧推流，用新 token 重推 |

---

## 前端决策逻辑（Auto 降级示例）

设备不做 Auto 降级，前端自行实现：

```
1. 前端尝试 P2P 连接
2. 收到 bye 消息 → P2P 失败/超时
3. 根据 bye reason 决定下一步：
   - timeout / negotiate_failed → RPC startLiveStream 切 SFU
   - p2p_occupied → 当前有人在看，等待或提示用户
   - sfu_active → 已经是 SFU 模式，直接 WHEP 拉流
```

---

## 配置字段

云端通过 ThingsBoard 下发的推流相关配置：

| 云端字段 | mediad 路径 | 说明 |
|---------|------------|------|
| `mqtt_signaling_url` | `live.mqtt_signaling.url` | 信令 MQTT 地址 |
| `push_stream_url` | `live.srs.whip_url_template` | WHIP 推流 URL 模板 |
| `ice_host` | `live.ice.host` | TURN 服务器地址 |
| `ice_port` | `live.ice.port` | TURN 服务器端口 |
| `ice_username` | `live.ice.username` | TURN 用户名 |
| `ice_password` | `live.ice.password` | TURN 密码 |

WHIP URL 模板支持占位符：
- `{deviceUid}` → 设备 ID
- `{accessToken}` → RPC 传入的 accessToken

示例：
```
http://srs.example.com/rtc/v1/whip/?app=live&stream={deviceUid}&secret={accessToken}
```
