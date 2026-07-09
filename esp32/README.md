# ESP32 Firmware

## Files

| File | Purpose |
|---|---|
| `main.ino` | Main sketch — setup(), loop(), task scheduling |
| `config.h` | WiFi SSID, MQTT endpoint, station_id, topic names |
| `secrets.h` | TLS certificates — DO NOT commit to git |
| `sensors.h/cpp` | Sensor reading, SmartShunt via UART |
| `mqtt_client.h/cpp` | MQTT connection, publish, subscribe, callback |
| `safety.h/cpp` | Local safety protections (runs even without cloud) |
| `actuators.h/cpp` | Relays, SSR, DC-DC outputs, linear actuators |

## Flash instructions

1. Install Arduino IDE 2.x
2. Add ESP32 board: https://dl.espressif.com/dl/package_esp32_index.json
3. Install libraries: PubSubClient, ArduinoJson, WiFiClientSecure
4. Copy your certificates into secrets.h
5. Set your WiFi and endpoint in config.h
6. Upload to board
