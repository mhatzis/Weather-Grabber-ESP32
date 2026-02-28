# Weather-Grabber-ESP32



ESP32-based irrigation/weather controller that pauses watering when significant rain is forecasted via OpenWeatherMap.  
Includes MQTT remote control, web configuration interface, OTA updates, and hardware reset button.

## Features

- Rain probability forecast check (OpenWeatherMap API 2.5 or 3.0)
- Configurable 24h / 48h forecast window and probability threshold
- MQTT commands:
  - `.../command/enable_rain` → "on" / "off" / "enable" / "disable"
  - `.../command/relay` → "on" / "off" / "auto"
- Publishes status, rain risk, max probability, high-risk slot count, etc.
- Web UI with authentication (default: admin / admin)
- OTA firmware upload via browser
- Long-press BOOT button (GPIO 0) for 10 seconds → resets WiFi credentials + web login to defaults
- Visual LED feedback on GPIO 23 during reset hold

## Hardware Requirements

- ESP32_Relay X1_V1.2
- https://www.aliexpress.com/item/1005008600008743.html?spm=a2g0o.productlist.main.29.3ab1y4zyy4zymY&algo_pvid=ff195084-5b32-4c09-a6aa-325568c7b2b3&algo_exp_id=ff195084-5b32-4c09-a6aa-325568c7b2b3-28&pdp_ext_f=%7B%22order%22%3A%2248%22%2C%22eval%22%3A%221%22%2C%22fromPage%22%3A%22search%22%7D&pdp_npi=6%40dis%21AUD%2113.05%219.79%21%21%219.13%216.85%21%4021033d9d17722355207238008e80c6%2112000049463553356%21sea%21AU%21100028349%21X%211%210%21n_tag%3A-29919%3Bd%3Ac6f2ab8c%3Bm03_new_user%3A-29895&curPageLogUid=qNV41cK0FuSu&utparam-url=scene%3Asearch%7Cquery_from%3A%7Cx_object_id%3A1005008600008743%7C_p_origin_prod%3A
- 
- Relay module (active LOW or HIGH – configurable)
- Optional: status LED on GPIO 23 
- GPIO 0 usually already has the BOOT button

## Wiring

- Relay IN → GPIO 16
- Reset button → GPIO 0 (to GND when pressed)
- Status LED → GPIO 23 → resistor → GND

## Installation

1. Open `weather_grabber.ino` in Arduino IDE
2. Install libraries:
   - WiFiManager
   - PubSubClient
   - (optional future: ArduinoJson)
3. Select your ESP32 board and port
4. Upload

On first boot → WiFi AP: **WeatherGrabber-AP** / password: **password123**

Default web login: **admin** / **admin**

Access settings at http://<device-ip>/settings

## MQTT Topics (default base = irrigation/control)

**Commands**  
`irrigation/control/command/enable_rain` → "on", "off", "enable", "disable"  
`irrigation/control/command/relay` → "on", "off", "auto", "weather"

**Status**  
`irrigation/control/status` → "allowed" / "blocked"  
`irrigation/control/status/rain_enabled` → "true" / "false"  
`irrigation/control/status/relay` → "on" / "off" / "auto"  
`irrigation/control/rain_risk` → "high" / "low"  
`irrigation/control/max_pop`, `high_slots`, `threshold`, `forecast_hours`, `last_message`, ...

## Reset to Factory Defaults

Hold **GPIO 0** (BOOT button) for **10 seconds**:
- WiFi credentials erased (re-enters AP mode)
- Web login reset to `admin` / `admin`
- LED on GPIO 23 provides visual feedback (solid → fast blink → confirmation blinks)

## License

MIT License – see [LICENSE](LICENSE) file

## Acknowledgments

Project assisted by Grok (xAI) – thank you!

Happy watering (or not watering) :)
