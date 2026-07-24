# 智巡精灵 v1.0 Handoff

## Current State

- 主分支基线: `091071c chore: trim vendor hardware resources`.
- Target: ESP32-2424S012, ESP32-C3 (单核 160 MHz), 4 MB flash.
- 屏幕: GC9A01 圆形 240×240, SPI (SCLK=6, MOSI=7, DC=2, CS=10), 80 MHz 写入.
- 触控: CST816D at I2C 0x15 (INT=GPIO0, SDA=4, SCL=5).
- 固件源码: `firmware/robot_inspection_panel/`.
- 后端模拟器: `backend/app.py`.
- 设备 MAC: `70:af:09:18:db:24`.
- 后端运行于 `http://192.168.0.35:8765`.
- 最近一次 STA 地址: `192.168.0.26`（由 DHCP 分配，不应写死）.
- 分区方案: `huge_app`（关闭 OTA，给应用 ~3 MB 空间）.
- 编译结果: 1,599,928 bytes Flash (50%), 42,852 bytes global RAM (13%).
- UI 仅使用 LovyanGFX 直接渲染，不再依赖或初始化 LVGL.
- 仓库工作内容约 7.9 MB（不含 `.git` 和本地 `.venv`）.

2026-07-24 最后验证时，后端健康检查正常；面板完成首页、点位滑动、选点和返回流程后，通过“配置网络”按钮进入 AP 配网模式。需要恢复 STA 时重新提交 Wi-Fi 和后端地址。

## 用户操作流程

1. 启动 → Wi-Fi 连接（或 AP 配网）→ 后台获取点位配置.
2. 首页点击“选择点位” → 进入可上下滑动的点位列表.
3. 点击某一点位 → 确认呼叫页面.
4. 确认 → 发送导航请求 → 轮询 `navigating` → `arrived`.
5. 到达后 → 选择“常规巡检” → 轮询 `running` → `completed`.
6. 完成巡检 → 回到首页.
7. 点击“配置网络”，或长按首页顶部 64 px 区域 5 秒 → 进入配网模式.

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
任务和幂等映射当前只保存在进程内存中，后端重启后会清空。

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

`config.h` 不再由 Git 跟踪。首次构建需从模板复制：

```bash
cp firmware/robot_inspection_panel/config.example.h \
  firmware/robot_inspection_panel/config.h
```

当前本机仍保留可编译的 `config.h`。Wi-Fi 凭据曾出现在旧 Git 历史中，应视为已泄露并轮换；普通删除提交不会缩小或清除历史对象。

### 仓库结构

| 路径 | 内容 |
| --- | --- |
| `backend/` | FastAPI 模拟后端及 Python 依赖清单 |
| `firmware/robot_inspection_panel/` | 当前可烧录固件 |
| `1.28inch_ESP32-2424S012/` | 规格书、结构图、芯片资料、原理图、用户手册及最小显示示例 |
| `README.md` | 快速启动说明 |
| `HANDOFF.md` | 架构、运行状态与排障交接 |

厂商目录中的第三方库副本、Factory/LVGL demo、Windows 工具、日志、烧录缓存和预编译固件已移除。旧对象仍存在于 Git 历史中，`.git` 目前约 122 MB；如需缩小克隆体积，必须另行执行历史重写并协调强制推送。

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

- **渲染引擎**：LovyanGFX 直接渲染。点位列表使用 176×170 的 2-bit 离屏画布一次性推送可视区，避免滑动时整屏闪烁。
- **内存策略**：点位画布只使用黑、深灰、青、白 4 色，约占 7.5 KB heap；曾尝试的 8-bit 画布会挤占 Wi-Fi 初始化内存，禁止恢复为大色深缓冲。
- **字体**：`efontCN_16`（中文字体），`setTextDatum(middle_center)` 居中绘制。
- **安全区**：圆形屏幕中，`circularSafeWidth(y)` 计算给定 y 坐标的安全水平宽度（根据圆方程 `2*sqrt(120²-(y-120)²)`），避免文字超出圆形边缘。
- **自适应字号**：`directText()` 在文字超宽时自动缩小字号或截断加省略号。
- **"配置网络"按钮**：`directButton()` 绘制圆角矩形 + 青色边框 + 白色文字。
- **QR 码**：使用 `esp_qrcode` 库生成 `WIFI:T:nopass;S:SmartInspect-XXXX;;` 格式二维码。渲染在屏幕中央约 112×112 px（含静区），scale=3，quiet=4。

### 各页面布局

| 页面 | 内容 |
| --- | --- |
| HOME | 站点名、Wi-Fi 状态、“选择点位”和“配置网络”入口 |
| POINT\_LIST | 可上下滑动的点位按钮列表 |
| NETWORK\_SETUP | "网络配置"标题、QR 码/热点名/IP 或验证状态或失败提示 |
| CONFIRM\_CALL | "确认呼叫"、点位名、取消/确认按钮 |
| WAIT\_ARRIVAL | 任务状态（"发送中"→"前往点位"→"已到达"） |
| PICK\_INSPECTION | "选择巡检类型"、可选列表 |
| INSPECTION\_STATUS | "巡检中"→"已完成"、"失败" |
| ERROR\_PAGE | 错误信息 |

### 触摸处理

- 原始触摸通过 `touch.getTouch(&x, &y, &gesture)` 读取。
- 普通页面使用 350 ms 触摸门限，按 y 坐标命中当前页按钮。
- 点位列表在松手时才区分点击与滑动：移动至少 8 px 视为拖动，否则视为选点。
- CST816D 短暂掉触使用 40 ms 容错；控制器只返回手势时，上/下滑会按 114 px 步进滚动。
- 打开点位页的那次按压必须先松手，不能被复用为列表点击。
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

## 已知约束

- 使用 `huge_app` 分区，因此当前不支持 OTA。
- `RobotAdapter` 是睡眠等待模拟。接入真实机器人时需要保留面板侧 API 和 `event_id` 幂等性。
- 新增页面内容时需继续通过 `circularSafeWidth()` 校验圆屏边界。
- 点位和巡检项分别最多加载 20 和 4 条，超出部分会被截断。
- Git 历史仍包含已删除的厂商大文件和旧 Wi-Fi 凭据；凭据需要轮换，历史瘦身需单独实施。

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

固件依赖：LovyanGFX、ArduinoJson、QRCode（`esp_qrcode`）。不需要 LVGL。

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

1. **后端状态页面** — 查看当前 NVS 配置、设备 ID、固件版本
2. **无线 OTA** — 需要调整当前 `huge_app` 分区方案
3. **真实机器人 HTTP 适配器** — 替换 `RobotAdapter` 模拟器
4. **多语言支持** — 后端巡检点位提供语言字段

## 最近完成

- `7a92a0e`: 点位列表点击/拖动分离，支持滑动选择更多点位。
- `33ab10c`: 移除未使用的 LVGL、旧页面构建器、双缓冲和 DMA 初始化。
- `091071c`: 精简厂商资源、停止跟踪敏感配置和 Python 缓存，统一文档端口为 `8765`。
