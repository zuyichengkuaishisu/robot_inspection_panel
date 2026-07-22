# Robot Inspection Panel Handoff

## Current State

- Target: ESP32-2424S012, ESP32-C3, 240x240 GC9A01 display, CST816D touch.
- Firmware source: `firmware/robot_inspection_panel/`.
- Backend source: `backend/app.py`.
- The panel was compiled and uploaded to `/dev/ttyACM0` successfully. The device MAC is `70:af:09:18:db:24`.
- The backend simulator is currently healthy on `http://192.168.0.35:8765`.
- The panel has joined the configured Wi-Fi and was observed at `192.168.0.26`.

## User Flow

1. Panel starts with a Wi-Fi connecting screen, then retrieves points.
2. Tap a point to open the call confirmation page.
3. Confirm the call. The panel immediately shows a sending state, then polls navigation.
4. On arrival, select `General inspection`.
5. The panel polls the inspection task through completion or failure.

## Backend API

| Endpoint | Purpose |
| --- | --- |
| `GET /health` | Health check |
| `GET /api/panels/{device_id}/config` | Point configuration |
| `GET /api/points/{point_id}/inspections` | Available inspections |
| `POST /api/navigation-tasks` | Create a robot navigation task |
| `POST /api/inspection-tasks` | Create an inspection after arrival |
| `GET /api/tasks/{task_id}` | Poll task status |

The simulator stores tasks in memory. It transitions navigation through `queued`, `navigating`, `arrived`, and inspections through `queued`, `running`, `completed`. `event_id` is idempotent.

## Firmware Notes

- `config.h` contains the local Wi-Fi and backend address. It is intentionally local configuration; do not copy its credentials into documentation or commits.
- The selected build target is:

```bash
esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app
```

- `huge_app` is required because the firmware exceeds the default app partition. It disables OTA partitions.
- The CST816D is present at I2C address `0x15`. Raw press/release and coordinate reports were verified by serial logs.
- UI state is rendered directly through LovyanGFX after state changes. This was added because the LVGL rendering path left stale frames on this hardware. LVGL remains initialized, but direct LovyanGFX rendering is the active display path.
- Raw touch dispatch has a 350 ms debounce because the touch controller can report brief release states during one physical press.

## Run Backend

```bash
cd /home/wzy/Arduino/robot_inspection_panel/backend
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8765
```

Open API documentation at `http://192.168.0.35:8765/docs` while that host address remains current. Update `BACKEND_URL` in `config.h` when the host LAN address changes.

## Compile And Upload

```bash
cd /home/wzy/Arduino/robot_inspection_panel
arduino-cli compile \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app \
  firmware/robot_inspection_panel

arduino-cli upload --verify \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app \
  -p /dev/ttyACM0 firmware/robot_inspection_panel
```

The host user must be in the `dialout` group to access `/dev/ttyACM0`.

## Serial Diagnostics

Use 115200 baud. Key messages:

- `CST816D I2C probe at 0x15: found`: touch controller is reachable.
- `Raw tap x=... y=... page=...`: a touch was routed to the app state machine.
- `UI page A -> B`: requested page transition.
- `HTTP ...`, `GET response code=...`, `POST response code=...`: backend transport state.
- `Task ... status ...`: simulated robot task progression.

## Known Follow-Up

- Confirm the latest direct-rendered UI is visibly responsive on the physical panel after the final firmware upload. Earlier LVGL DMA and synchronous flush attempts displayed stale pages despite correct state-machine logs.
- Replace `RobotAdapter` in `backend/app.py` with the real robot HTTP implementation when its control protocol is available. Preserve the panel-facing API and event-ID idempotency.
