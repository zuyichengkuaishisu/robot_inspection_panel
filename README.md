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

The panel scans for nearby 2.4 GHz Wi-Fi networks for 8 seconds on boot. If a previously saved configuration exists in NVS, it attempts to connect directly. Otherwise, or if the saved network is unreachable, it starts an open AP hotspot named `SmartInspect-XXXX` (where `XXXX` is the last four hex digits of the device MAC).

Connect a phone to the hotspot. The panel displays a QR code for easy joining, and the captive portal automatically opens `http://192.168.4.1/`. The web page lists all scanned networks with signal strength, allows manual SSID entry, and collects the Wi-Fi password (leave blank for an open network) and the backend HTTP URL.

After submitting, the panel switches to STA mode, connects to the target network, and checks the backend `/health` endpoint. On success the configuration is saved to NVS and the panel restarts; on failure the AP portal remains open for another attempt. A backend warning does not prevent saving the Wi-Fi configuration.

The panel operates in **pure AP-only mode** during provisioning — no mixed STA/AP mode. After a successful provision it restarts and connects as a Wi-Fi station. To reprovision from an already-connected state, hold the title bar for five seconds on the home page.

## Firmware

Edit `firmware/robot_inspection_panel/config.h`, especially `BACKEND_URL` (use the LAN IP of the backend host). The `WIFI_SSID` / `WIFI_PASSWORD` values in config.h are only used as a first-boot fallback; after the first web provisioning, credentials are stored in NVS.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app firmware/robot_inspection_panel
```

Required Arduino libraries are LovyanGFX, LVGL 9, ArduinoJson and QRCode. `huge_app` is required because the UI and Wi-Fi HTTP client exceed the default 1.2 MB application partition; it disables OTA updates. The panel flow is point selection, call confirmation, arrival polling, inspection selection, then inspection polling.
