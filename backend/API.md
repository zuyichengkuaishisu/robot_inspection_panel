# 智巡精灵 FastAPI 接口说明

本文档对应当前 `backend/app.py` 的 FastAPI 服务，适用于 ESP32 面板和其他局域网客户端。

## 1. 服务信息

- 默认地址：`http://<backend-host>:8765`
- Swagger UI：`http://<backend-host>:8765/docs`
- OpenAPI JSON：`http://<backend-host>:8765/openapi.json`
- 数据存储：内存。后端重启后任务记录和机器人状态会重置。
- 当前模型：单机器狗、全局单活动任务。

### 认证

默认不启用认证。当设置环境变量 `ROBOT_PANEL_API_TOKEN` 后，所有 `/api/*` 接口都必须携带请求头：

```http
X-API-Token: <token>
```

认证失败返回 `401`。`/health` 不需要认证。

## 2. 通用数据结构

### 点位 `Point`

```json
{
  "id": "warehouse",
  "name": "仓库"
}
```

### 任务 `Task`

所有任务接口都返回以下字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | string | 后端生成的任务 ID |
| `event_id` | string | 客户端事件 ID，用于幂等 |
| `kind` | string | `navigate`、`delivery`、`inspection` |
| `status` | string | 任务当前状态 |
| `message` | string | 面向用户的状态说明 |
| `device_id` | string | 面板或客户端设备 ID |
| `point_id` | string | 任务目标点位 ID |
| `purpose` | string/null | 导航用途：`delivery_pickup` 或 `inspection` |
| `parent_task_id` | string/null | 关联的前置任务 ID |
| `source_point_id` | string/null | 送货起点 |
| `destination_point_id` | string/null | 送货终点 |
| `unload_deadline` | number/null | Unix 时间戳；等待卸货截止时间 |
| `unload_remaining_seconds` | integer/null | 剩余卸货秒数 |
| `created_at` | number | 创建时间，Unix 秒 |
| `updated_at` | number | 更新时间，Unix 秒 |

任务状态包括：

`queued`、`navigating`、`arrived`、`awaiting_load`、`awaiting_destination`、`delivering`、`awaiting_unload`、`running`、`completed`、`failed`、`cancelled`。

## 3. 接口列表

### `GET /health`

健康检查，无需认证。

响应：

```json
{"ok": true}
```

### `GET /api/robot/status`

获取机器人全局状态，建议面板每秒轮询一次。

响应示例（空闲）：

```json
{
  "state": "idle",
  "current_point": {"id": "lobby", "name": "大厅"},
  "target_point": null,
  "phase": "idle",
  "message": "机器狗空闲",
  "active_task": null,
  "unload_deadline": null,
  "unload_remaining_seconds": null,
  "updated_at": 1710000000.0
}
```

`state` 为 `idle` 或 `busy`。机器人移动时 `current_point` 保留最后已知位置，`target_point` 表示当前目标点。

### `GET /api/panels/{device_id}/config`

获取面板站点信息和点位列表。

响应字段：`device_id`、`site_id`、`site_name`、`points`。

### `GET /api/points/{point_id}/inspections`

获取指定点位可用的巡检项目。

点位不存在返回 `404`。

响应示例：

```json
{
  "point_id": "line-1",
  "inspections": [{"id": "general", "name": "常规巡检"}]
}
```

### `POST /api/navigation-tasks`

创建导航任务。用于送货召唤点或巡检点。

请求体：

```json
{
  "event_id": "nav-001",
  "device_id": "panel-001",
  "site_id": "site-a",
  "point_id": "warehouse",
  "purpose": "delivery_pickup",
  "replace_task_id": null
}
```

`purpose` 可选值：

- `delivery_pickup`：前往送货召唤点，到达后进入 `awaiting_load`
- `inspection`：前往巡检点，到达后进入 `arrived`

机器人忙碌时，未提供当前活动任务 ID 会返回 `409`；覆盖任务时必须将当前任务 ID 填入 `replace_task_id`。覆盖会取消旧任务并创建新任务，旧异步执行器也会被取消。

成功返回 `201` 和任务对象。点位不存在返回 `422`。

### `POST /api/navigation-tasks/{task_id}/load-complete`

确认送货召唤点已装货，使导航任务从 `awaiting_load` 进入 `awaiting_destination`。

请求体：

```json
{
  "event_id": "load-001",
  "device_id": "panel-001"
}
```

任务不存在或不是送货召唤任务返回 `404`；任务未处于等待装货状态返回 `409`。

### `POST /api/delivery-tasks`

创建送货任务。召唤导航必须已经完成装货确认。

请求体：

```json
{
  "event_id": "delivery-001",
  "device_id": "panel-001",
  "pickup_task_id": "<召唤导航任务 ID>",
  "destination_point_id": "line-1"
}
```

送货点不能与召唤点相同；相同点位返回 `422`。成功返回 `201`，任务状态从 `queued` 开始，随后进入 `delivering` 和 `awaiting_unload`。

### `POST /api/delivery-tasks/{task_id}/unload-complete`

人工确认卸货完成。仅允许对处于 `awaiting_unload` 的送货任务调用。

请求体与装货确认相同：

```json
{
  "event_id": "unload-001",
  "device_id": "panel-001"
}
```

成功后任务变为 `completed`，机器人回到全局空闲状态。未处于等待卸货状态返回 `409`。

### `POST /api/inspection-tasks`

创建巡检执行任务。巡检导航必须已经到达目标点。

请求体：

```json
{
  "event_id": "inspection-001",
  "device_id": "panel-001",
  "arrival_task_id": "<巡检导航任务 ID>",
  "inspection_id": "general"
}
```

成功后导航任务结束并创建 `kind=inspection` 的活动任务，任务随后进入 `running`，完成后为 `completed`。巡检项目不存在返回 `422`，尚未到达返回 `409`。

### `GET /api/tasks/{task_id}`

查询单个任务的最新状态。任务不存在返回 `404`。

## 4. 幂等与错误处理

所有写接口都要求非空 `event_id`（最长 96 个字符）。同一 `event_id` 再次请求时，服务直接返回第一次请求关联的任务，不会重复创建或重复执行。

常见 HTTP 状态码：

| 状态码 | 含义 |
| --- | --- |
| `201` | 任务创建成功 |
| `401` | API Token 缺失或错误 |
| `404` | 任务、点位或资源不存在 |
| `409` | 机器人忙碌、任务阶段不匹配或覆盖 ID 失效 |
| `422` | 请求字段校验失败、未知点位或非法送货点 |

错误响应通常为 FastAPI 的 JSON 错误对象，例如：

```json
{"detail": "robot is not awaiting load for this task"}
```

机器人忙碌时的覆盖冲突响应会在 `detail` 中附带当前活动任务：

```json
{
  "detail": {
    "message": "robot busy",
    "active_task": {"id": "...", "status": "navigating"}
  }
}
```

## 5. 任务流程

### 送货

```text
POST navigation(purpose=delivery_pickup)
  -> awaiting_load
POST load-complete
  -> awaiting_destination
POST delivery-tasks
  -> delivering -> awaiting_unload
POST unload-complete 或等待超时
  -> completed -> idle
```

### 巡检

```text
POST navigation(purpose=inspection)
  -> arrived
POST inspection-tasks
  -> running -> completed -> idle
```

## 6. 模拟参数

| 环境变量 | 默认值 | 说明 |
| --- | ---: | --- |
| `ROBOT_SIM_STEP_SECONDS` | `1` | 模拟机器人阶段间隔 |
| `DELIVERY_UNLOAD_TIMEOUT_SECONDS` | `120` | 等待卸货超时时间 |
| `ROBOT_PANEL_API_TOKEN` | 空 | 非空时启用 `X-API-Token` 校验 |

测试时可缩短流程，例如：

```bash
ROBOT_SIM_STEP_SECONDS=0.05 DELIVERY_UNLOAD_TIMEOUT_SECONDS=0.2 \
  uvicorn app:app --host 0.0.0.0 --port 8765
```

## 7. curl 示例

```bash
BASE=http://127.0.0.1:8765

# 查看机器人状态
curl "$BASE/api/robot/status"

# 创建送货召唤
curl -X POST "$BASE/api/navigation-tasks" \
  -H 'Content-Type: application/json' \
  -d '{"event_id":"nav-001","device_id":"panel-001","site_id":"site-a","point_id":"warehouse","purpose":"delivery_pickup"}'

# 装货完成（替换 TASK_ID）
curl -X POST "$BASE/api/navigation-tasks/TASK_ID/load-complete" \
  -H 'Content-Type: application/json' \
  -d '{"event_id":"load-001","device_id":"panel-001"}'

# 创建送货（替换 PICKUP_TASK_ID）
curl -X POST "$BASE/api/delivery-tasks" \
  -H 'Content-Type: application/json' \
  -d '{"event_id":"delivery-001","device_id":"panel-001","pickup_task_id":"PICKUP_TASK_ID","destination_point_id":"line-1"}'
```
