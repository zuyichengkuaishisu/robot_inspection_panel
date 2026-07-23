# 智巡精灵 v1.0

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

## Wi-Fi Provisioning

The panel first scans for nearby networks, then tries the saved Wi-Fi configuration for 15 seconds. If no usable configuration exists or the connection fails, it starts an open hotspot named `SmartInspect-XXXX`, where `XXXX` is derived from the device MAC address.

Connect a phone to that hotspot and open `http://192.168.4.1/`. The panel also displays a QR code for joining the hotspot. The page can scan nearby Wi-Fi networks, configure a hidden SSID manually, and set the backend HTTP URL. Credentials are tested before they are written to NVS; a failed test leaves the portal available for another attempt.

When the panel is already online, hold the `智巡精灵 v1.0` title for five seconds to enter provisioning mode again. Existing `config.h` values are used as a migration fallback until a web configuration is saved.

Once provisioning mode starts, that state is stored in NVS. Resetting the panel keeps the hotspot and portal active until a new Wi-Fi configuration has been verified and saved successfully.

## Firmware

Edit `firmware/robot_inspection_panel/config.h`, especially `BACKEND_URL` (use the LAN IP of the backend host), then compile:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app firmware/robot_inspection_panel
```

Required Arduino libraries are LovyanGFX, LVGL 9 and ArduinoJson. `huge_app` is required because the UI and Wi-Fi HTTP client exceed the default 1.2 MB application partition; it disables OTA updates. The panel flow is point selection, call confirmation, arrival polling, inspection selection, then inspection polling.
