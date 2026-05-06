# AGENTS.md - ESP32 Irrigation System

## Build & Upload

```bash
# Build specific environment
pio run -e esp32dev_ota    # OTA version (default)
pio run -e esp32dev_usb   # USB version

# Upload (uses platformio.ini settings)
pio run -e esp32dev_ota --target upload
pio run -e esp32dev_usb --target upload

# Monitor serial output
pio device monitor
```

## Environments

| Env | Upload | Monitor Port |
|-----|--------|------------|
| esp32dev_ota | OTA @ 192.168.178.159:3232 | COM4 |
| esp32dev_usb | USB | COM4 |

## Key Files

- `src/main.cpp` - Main application (~2250 lines)
- `src/config.h` - Hardware/config constants
- `src/secrets.h` - **WiFi, Telegram bot tokens, API keys** (create from template)

## First-Time Setup

1. Create `src/secrets.h` with:
   - `SECRET_WIFI_SSID`, `SECRET_WIFI_PASS`
   - `SECRET_BOT_TOKEN` (Telegram)
   - `SECRET_CHAT_ID` (authorized user)
   - `SECRET_API_OPENWEATHER` (OpenWeatherMap)

## Debug

- VS Code: Use "PIO Debug" or "PIO Debug (skip Pre-Debug)" configurations
- Telnet commands (port 23): `tail`, `alert`, `health`, `clear`, `size`

## Notes

- Two production variants exist: `src/main.cpp` (simple) and `test/main.cpp` (FreeRTOS tasks)
- Both share similar logic/integration tests needreal hardware (ESP32 + sensors)

# Project rules

- Progetto: ESP32 irrigation controller con PlatformIO.
- Obiettivo attuale: stabilizzare il refactor da main.cpp monolitico a moduli separati.
- Priorità: compilazione pulita > refactor estetico.
- Non introdurre nuove astrazioni se non necessarie.
- Non duplicare prototipi nei .cpp.
- Evitare inline negli header se esiste già una definizione .cpp.
- Uniformare i tipi shared tra extern e definizioni.
- Ogni modifica va verificata con build.
- Procedere per micro-step.