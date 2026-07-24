# 智巡精灵 v1.0 Handoff

## Current State

- Target: ESP32-2424S012, ESP32-C3 (单核 160 MHz), 4 MB flash.
- 屏幕: GC9A01 圆形 240×240, SPI (SCLK=6, MOSI=7, DC=2, CS=10), 80 MHz 写入.
- 触控: CST816D at I2C 0x15 (INT=GPIO0, SDA=4, SCL=5).
- 固件源码: `firmware/robot_inspection_panel/`.
- 后端模拟器: `backend/app.py`.
- 设备 MAC: `70:af:09:18:db:24`.
- 后端运行于 `http://192.168.0.35:8765`.
- 配网成功后面板 IP: `192.168.0.26`.
- 分区方案: `huge_app`（关闭 OTA，给应用 ~3 MB 空间）.
- Flash 使用: ~59%, RAM: ~63%.

## 用户操作流程

1. 启动 → Wi-Fi 连接（或 AP 配网）→ 获取点位列表.
2. 点击某一点位 → 确认呼叫页面.
3. 确认 → 发送导航请求 → 轮询 `navigating` → `arrived`.
4. 到达后 → 选择"常规巡检" → 轮询 `running` → `completed`.
5. 完成巡检 → 回到首页.
6. **长按首页标题 `智巡精灵 v1.0` 5 秒** → 进入配网模式.

## 后端 API

| Endpoint | 用途 |
| --- | --- |
| `GET /health` | 健康检查 |
| `GET /api/panels/{device_id}/config` | 获取点位配置 |
| `GET /api/points/{point_id}/inspections` | 获取可选巡检项 |
| `POST /api/navigation-tasks` | 创建导航任务 |
| `POST /api/inspection-tasks` | 创建巡检任务 |
| `GET /api/tasks/{task_id}` | 轮询任务状态 |

后端支持 `X-API-Token` Header 鉴权（通过环境变量 `ROBOT_PANEL_API_TOKEN` 设置）。
模拟器通过 `RobotAdapter` 异步模拟机器人行为：导航 `queued→navigating→arrived`（~4s），巡检 `queued→running→completed`（~4s）。
`event_id` 幂等：相同 event_id 重复请求返回已有任务。

## 固件架构

### 文件结构

| 文件 | 职责 |
| --- | --- |
| `robot_inspection_panel.ino` | 主程序：setup/loop、触摸分发、UI 渲染、业务状态机 |
| `ProvisioningPortal.h` | Wi-Fi 配网类声明 |
| `ProvisioningPortal.cpp` | 配网状态机：扫描、连接、AP、WebServer、DNS、NVS 持久化 |
| `CST816D.cpp` / `.h` | 触控控制器驱动 |
| `config.h` | 编译时后备 Wi-Fi/后端配置（git 忽略敏感值） |
| `config.example.h` | 配置模板 |

### 配网状态机（ProvisioningPortal）

状态流转：

```
启动
 ├─ NVS 有已保存凭据 → PROVISION_SCANNING → WiFi.begin()
 │   ├─ 12秒内连接成功 → PROVISION_CONNECTING → 正常巡检
 │   └─ 超时 → 扫描 8s → PROVISION_AP
 └─ NVS 无凭据 → 扫描 8s → PROVISION_AP (纯 AP 模式)
     
PROVISION_AP (纯 AP 模式 SmartInspect-XXXX)
 ├─ 网页提交配置 → PROVISION_VERIFYING
 │   ├─ 连接目标 Wi-Fi + 后端可达 → 写入 NVS → PROVISION_CONNECTING → 2.2秒后重启
 │   ├─ 连接目标 Wi-Fi + 后端不可达 → 写入 NVS（显示警告）→ PROVISION_CONNECTING → 重启
 │   └─ 15秒连接超时 → PROVISION_FAILED (热点保持在线，可重新填写)
 └─ [网页刷新 / 重新扫描]
```

关键设计点：

1. **纯 AP 模式**：使用 `WIFI_AP` 而非 `WIFI_AP_STA`，消除信道冲突和二次连接问题。验证时临时切到 `WIFI_STA`，验证失败后恢复 `WIFI_AP`。

2. **启动流程**：读取 NVS → 有保存凭据则 `WiFi.begin()` 尝试连接（约 12 秒超时）→ 无凭据或失败则扫描 8 秒后启动热点。

3. **`startPortal()`**：`WiFi.mode(WIFI_AP)` → 配置 `192.168.4.1/24` → 用 MAC 后 4 位生成 SSID → 启动 DNS 泛解析 + WebServer。此过程完全断开 STA 连接。

4. **验证流程**：网页提交后 → 切换到 `WIFI_STA` → 尝试连接目标 Wi-Fi → 连接成功后 HTTP GET `/health` → 后端可达则写 NVS 重启，不可达也写 NVS（警告）→ 超时则恢复 AP 模式。

5. **"已连接"状态**：连接成功或使用已有凭据连接后进入 `PROVISION_CONNECTING` 状态。巡检业务仅在 `isConnected()` 且 `WiFi.status() == WL_CONNECTED` 时工作。

### AP 与 Captive Portal

- 热点: 开放, channel 1, 最大 4 个客户端。
- DNSServer 将所有域名指向 `192.168.4.1`。
- WebServer (端口 80) 路由:

| 路由 | 方法 | 用途 |
| --- | --- | --- |
| `/` | GET | 中文配网页面 (HTML) |
| `/api/networks` | GET | 扫描到的 Wi-Fi 列表 (JSON) |
| `/api/status` | GET | 当前状态、消息、后端健康度 |
| `/api/config` | GET | 当前后端地址、设备 ID、热点名 |
| `/api/rescan` | POST | 触发 Wi-Fi 重新扫描 |
| `/api/provision` | POST | 提交 Wi-Fi 配置 (JSON: ssid/password/backend_url) |
| 任意其他路径 | * | 302 重定向到 `http://192.168.4.1/` |

- **Captive Portal 兼容**：DNS 泛解析 + 302 重定向，覆盖 Android/iOS/Windows 的连通性探测（如 `connectivitycheck.gstatic.com`、`captive.apple.com` 等）。手机连接热点后自动弹出配置页面。

- **配网页面前端**：内嵌单页 HTML+JS，深色主题，显示 Wi-Fi 列表（名称/信号/加密状态）、隐藏 SSID 手动输入、密码（开放网络可留空）、后端地址输入。"验证并保存"按钮提交后，前端轮询 `/api/status` 直到验证完成。

- **安全**：密码不回显，不写日志。SSID 1-32 字节，非空密码 8-63 字节，后端地址必须是 `http://` 前缀且不超过 160 字符。输出做 JSON/HTML 转义。

### 圆屏 UI 渲染

- **渲染引擎**：LovyanGFX 直接渲染（LVGL 初始化但未用于实际页面）。原因是 LVGL 路径在 GC9A01 上留下残影。
- **字体**：`efontCN_16`（中文字体），`setTextDatum(middle_center)` 居中绘制。
- **安全区**：圆形屏幕中，`circularSafeWidth(y)` 计算给定 y 坐标的安全水平宽度（根据圆方程 `2*sqrt(120²-(y-120)²)`），避免文字超出圆形边缘。
- **自适应字号**：`directText()` 在文字超宽时自动缩小字号或截断加省略号。
- **"配置网络"按钮**：`directButton()` 绘制圆角矩形 + 青色边框 + 白色文字。
- **QR 码**：使用 `esp_qrcode` 库生成 `WIFI:T:nopass;S:SmartInspect-XXXX;;` 格式二维码。渲染在屏幕中央约 112×112 px（含静区），scale=3，quiet=4。

### 各页面布局

| 页面 | 内容 |
| --- | --- |
| HOME | 标题"智巡精灵 v1.0"（青色）、Wi-Fi 状态、点位列表（按钮式）或"配置网络"按钮 |
| NETWORK\_SETUP | "网络配置"标题、QR 码/热点名/IP 或验证状态或失败提示 |
| CONFIRM\_CALL | "确认呼叫"、点位名、取消/确认按钮 |
| WAIT\_ARRIVAL | 任务状态（"发送中"→"前往点位"→"已到达"） |
| PICK\_INSPECTION | "选择巡检类型"、可选列表 |
| INSPECTION\_STATUS | "巡检中"→"已完成"、"失败" |
| ERROR\_PAGE | 错误信息 |

### 触摸处理

- 原始触摸通过 `touch.getTouch(&x, &y, &gesture)` 读取。
- 350 ms 消抖（物理按下期间可能短暂报告释放）。
- 触摸事件映射到按键区域：每页定义 `handleRawTap()` 的 `targetRects[]`。
- 首页顶部 64px 区域检测长按 5 秒进入配网。
- 后台每 250ms 输出诊断日志。

### 串口诊断（115200 baud）

关键日志：
- `CST816D I2C probe at 0x15: found` — 触控正常
- `Raw tap x=... y=... page=...` — 触摸路由
- `UI page A -> B` — 页面切换
- `HTTP GET ...` / `POST response code=...` — 后端通信
- `Task ... status ...` — 任务状态更新
- `Config: device=... site=... backend=...` — 启动配置
- Wi-Fi 连接后输出 IP 和 RSSI

**注意**：`Serial.printf` 在 USB CDC 模式下可能阻塞，所有日志语句都已检查 `Serial.availableForWrite()`。

## 已知问题与待修复

### 1. **配网热点二次连接问题** ✅ 已解决

**解决方案**：切换为 **纯 AP 模式**（`WIFI_AP`），消除 `WIFI_AP_STA` 混合模式下的信道冲突和 STA 自动重连干扰。

变更要点：
- 启动时扫描 8 秒，无已保存配置则自动进入纯 AP 模式
- 验证凭据时临时切到 `WIFI_STA` 测试连接，失败后恢复 `WIFI_AP`
- 移除 NVS `portal` 持久化标志，不再跨重启保持配网状态
- 连接成功后写入 NVS 并重启进入 STA 模式

### 2. **渲染性能与内容裁切**

- 已优化：`tft.startWrite()` 在 `setup()` 中保持 SPI 事务打开，避免首帧 2-3 秒阻塞。
- 部分文字在圆形边界附近的 y 坐标上可能超出屏幕边缘（用户曾反馈"检查显示内容，有的已经超过边缘"）。`circularSafeWidth()` 需要验证各页面 y 值对应的安全宽度。

### 3. **后端模拟器替换**

当前 `RobotAdapter` 是睡眠等待模拟。接入真实机器人时需要保留面板侧 API 和 event_id 幂等性。

## 编译与烧录

```bash
# 编译
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app \
  firmware/robot_inspection_panel

# 烧录
arduino-cli upload --verify \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app \
  -p /dev/ttyACM0 firmware/robot_inspection_panel
```

用户需属于 `dialout` 组以访问 `/dev/ttyACM0`。

## 运行后端

```bash
cd /home/wzy/Arduino/robot_inspection_panel/backend
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8765
```

Swagger 文档：`http://192.168.0.35:8765/docs`。后端地址变化时同步更新 `config.h` 中的 `BACKEND_URL`。

## 下一步功能

1. **修复配网热点二次连接问题**（当前阻塞项）
2. **后端状态页面** — 查看当前 NVS 配置、设备 ID、固件版本
3. **无线 OTA** — 免 USB 更新固件
4. **真实机器人 HTTP 适配器** — 替换 `RobotAdapter` 模拟器
5. **多语言支持** — 后端巡检点位提供 `name_zh` 字段

## Git 状态

- 最新提交: `5f0d4aa docs: 新增机器人巡检面板交接文档`
- 未提交修改: `backend/app.py`, `firmware/.../robot_inspection_panel.ino`, `README.md`, `HANDOFF.md` + 新增 `ProvisioningPortal.h`, `ProvisioningPortal.cpp`
- 分支前缀: `codex/`
