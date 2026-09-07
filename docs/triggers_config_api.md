# triggers 配置协议

## 概述

`triggers` 是一个 JSON 数组，配置设备的自动触发任务（定时拍照、时间段录像、蓝牙拍照等）。

通过 ThingsBoard 共享属性（MQTT）下发，设备端热更新生效，无需重启。

---

## JSON 结构

```json
{
  "triggers": [
    { "触发源1" },
    { "触发源2" },
    ...
  ]
}
```

---

## 字段定义

### 通用字段（所有 type 共用）

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | string | ✅ | 唯一标识，如 `"timer_1"`、`"record_work"` |
| `type` | string | ✅ | 触发源类型：`"timer"` / `"record"` / `"bluetooth"` |
| `enabled` | boolean | ✅ | `true` 启用 / `false` 禁用 |
| `priority` | number | — | 优先级，数字越大越高，默认 `0` |
| `all_day` | boolean | — | 是否全天生效，默认 `false`。`true` 时忽略 `schedule` |
| `schedule` | object | — | 时间表，`all_day=false` 时生效。不填 = 全天生效 |
| `schedule.start_time` | string | — | 起始时间，格式 `"HH:MM"` |
| `schedule.end_time` | string | — | 结束时间，格式 `"HH:MM"` |
| `schedule.days` | number[] | — | 生效的星期几，`0`=周日，`1`=周一，...，`6`=周六 |

### type=timer 专用字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `interval_sec` | number | ✅ | 拍照间隔（秒） |
| `burst_count` | number | — | 每次拍几张，默认 `1` |
| `burst_interval_ms` | number | — | 连拍间隔（毫秒），默认 `0` |

### type=record 专用字段

无额外字段。通过 `schedule` 控制录像时间段，系统自动管理开始/停止。

### type=bluetooth 专用字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `burst_count` | number | — | 每个标签拍几张，默认 `1` |
| `burst_interval_ms` | number | — | 连拍间隔（毫秒），默认 `0` |

蓝牙串口路径和波特率是硬件固定值，不需要配置。

---

## type 说明

| type | 做什么 | 触发方式 |
|------|--------|----------|
| `"timer"` | **拍照** | 每隔 `interval_sec` 秒拍一次，受 `schedule` 约束 |
| `"record"` | **录像** | 进入时间段自动开始，离开自动停止 |
| `"bluetooth"` | **拍照** | 蓝牙 tag 进入范围时触发 |

**动作由 type 决定，不需要单独配置。**

---

## 各 type 示例

### timer — 定时拍照

```json
{
  "id": "work_hours_snapshot",
  "type": "timer",
  "enabled": true,
  "all_day": false,
  "interval_sec": 120,
  "burst_count": 3,
  "burst_interval_ms": 500,
  "schedule": {
    "start_time": "09:00",
    "end_time": "18:00",
    "days": [1, 2, 3, 4, 5]
  }
}
```

> 工作日 9:00~18:00，每 2 分钟连拍 3 张（每张间隔 0.5 秒）

### record — 时间段录像

```json
{
  "id": "work_hours_record",
  "type": "record",
  "enabled": true,
  "all_day": false,
  "priority": 50,
  "schedule": {
    "start_time": "09:00",
    "end_time": "17:00",
    "days": [1, 2, 3, 4, 5]
  }
}
```

> 工作日 9:00~17:00 自动录像，其他时间不录。系统自动管理开始/停止。

全天录像：

```json
{
  "id": "allday_record",
  "type": "record",
  "enabled": true,
  "priority": 50,
  "all_day": true
}
```

### bluetooth — 蓝牙拍照

```json
{
  "id": "bt_snapshot",
  "type": "bluetooth",
  "enabled": true,
  "all_day": true,
  "priority": 30,
  "burst_count": 5,
  "burst_interval_ms": 2000
}
```

> 蓝牙 tag 进入范围时连拍 5 张（每张间隔 2 秒）

---

## 完整示例

```json
{
  "triggers": [
    {
      "id": "allday_snapshot",
      "type": "timer",
      "enabled": true,
      "all_day": true,
      "interval_sec": 600,
      "burst_count": 1
    },
    {
      "id": "work_hours_snapshot",
      "type": "timer",
      "enabled": true,
      "all_day": false,
      "interval_sec": 120,
      "burst_count": 3,
      "burst_interval_ms": 500,
      "schedule": {
        "start_time": "09:00",
        "end_time": "18:00",
        "days": [1, 2, 3, 4, 5]
      }
    },
    {
      "id": "allday_record",
      "type": "record",
      "enabled": true,
      "priority": 50,
      "all_day": true
    },
    {
      "id": "work_hours_record",
      "type": "record",
      "enabled": true,
      "priority": 50,
      "schedule": {
        "start_time": "09:00",
        "end_time": "17:00",
        "days": [1, 2, 3, 4, 5]
      }
    },
    {
      "id": "bt_snapshot",
      "type": "bluetooth",
      "enabled": true,
      "priority": 30,
      "burst_count": 5,
      "burst_interval_ms": 2000
    }
  ]
}
```

| 触发源 | 行为 |
|--------|------|
| `allday_snapshot` | 全天每 10 分钟拍 1 张 |
| `work_hours_snapshot` | 工作日 9~18 点，每 2 分钟连拍 3 张 |
| `allday_record` | 全天 24 小时录像 |
| `work_hours_record` | 工作日 9~17 点录像 |
| `bt_snapshot` | 蓝牙 tag 触发连拍 5 张 |

---

## 注意事项

1. **全量下发**：每次更新下发完整 `triggers` 数组，设备端清空旧配置后重建
2. **id 唯一**：`id` 必须唯一，重复会覆盖
3. **全天生效**：`all_day=true` 时忽略 `schedule`；`all_day=false` 时配合 `schedule` 使用
4. **days 空数组**：`[]` = 任何天都不生效（等于禁用）
5. **跨午夜**：`start_time > end_time` 自动处理跨午夜，如 `"22:00"~"06:00"`

---

## 优先级抢占

多个触发源同时触发时，按优先级抢占：

| 优先级 | 建议值 | 场景 |
|--------|--------|------|
| 最高 | 100 | SOS 紧急按钮 |
| 高 | 50 | 运动检测 |
| 中 | 30 | 蓝牙 tag |
| 低 | 10 | 定时任务 |

- 高优先级抢占低优先级，被抢占的丢弃不排队
- 同等优先级：先来先执行，后来的丢弃
