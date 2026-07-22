# ESP32 Robot Inspection Panel

Independent project for the ESP32-2424S012 (240x240 GC9A01 display, CST816D touch).

## Backend

```bash
cd backend
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8000
```

Set `ROBOT_PANEL_API_TOKEN` only when a panel token is also configured in `firmware/robot_inspection_panel/config.h`.

## Firmware

Edit `firmware/robot_inspection_panel/config.h`, especially `BACKEND_URL` (use the LAN IP of the backend host), then compile:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app firmware/robot_inspection_panel
```

Required Arduino libraries are LovyanGFX, LVGL 9 and ArduinoJson. `huge_app` is required because the UI and Wi-Fi HTTP client exceed the default 1.2 MB application partition; it disables OTA updates. The panel flow is point selection, call confirmation, arrival polling, inspection selection, then inspection polling.
