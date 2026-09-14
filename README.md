# NanoInk

[中文版本](README.zh-CN.md)

NanoInk is a personal ESP32 e-paper information terminal for a daily class schedule, weather, time, battery voltage and a TOTP code. It combines a DS3231 clock, a locally cached timetable and periodic deep sleep.

## Hardware and operation

The Arduino board selection is **ESP32 Dev Module** (`esp32:esp32:esp32`). The display driver is GxEPD2 `GxEPD2_213_BN`, with a rotated 250 × 128 drawing area. Verify the physical panel matches this driver before connecting it.

| Connection | ESP32 GPIO |
| --- | --- |
| E-paper SCK / MISO / MOSI | 13 / 12 / 14 |
| E-paper CS / DC / RST / BUSY | 15 / 27 / 26 / 25 |
| DS3231 I²C | Board-default SDA / SCL |
| Battery measurement input | 36 |

The battery conversion currently uses `ADC × 1.1 / 455`. The divider, supply circuit and calibration are specific to the original hardware and have not been documented or remeasured. Do not connect battery voltage directly to the ADC without checking the circuit and input limits.

On first boot, the device attempts Wi-Fi, falls back to the configured hotspot, synchronizes time and downloads data. It subsequently wakes approximately once per minute, uses partial display updates except at half-hour boundaries, and attempts network updates at the top of each hour. NTP synchronization is scheduled for noon, and the timetable is refreshed on Sunday at noon. An RTC that reports lost power also triggers a time synchronization attempt during an hourly connection.

The DS3231 and class timestamps use **UTC+8 local wall time**. TOTP and the TLS system clock use UTC. The firmware reads the first unexpired class from the cache; missing or exhausted schedules show no class. A failed or invalid download preserves the previous timetable. SPIFFS updates use a temporary file and backup because SPIFFS cannot rename onto an existing destination. This has not been verified as power-failure-safe on the physical device. Initial SPIFFS mounting retains Arduino's format-on-mount-failure behavior.

## Build and configuration

Install the ESP32 board package using this Boards Manager URL:

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

Use ESP32 core **3.3.5** and these Library Manager versions:

| Library | Version |
| --- | --- |
| ArduinoJson | 7.4.2 |
| GxEPD2 | 1.6.5 |
| NTPClient | 3.2.1 |
| RTClib | 2.1.4 |
| TOTP library | 1.1.0 |
| U8g2 | 2.35.30 |
| U8g2_for_Adafruit_GFX | 1.8.0 |
| Adafruit GFX Library | 1.12.4 |
| Adafruit BusIO | 1.17.4 |

1. Copy `config.example.h` to `config.local.h` in the sketch directory.
2. Enter your network settings, API URLs, weather key/location and TOTP seed. The example TOTP seed is a public test vector and must not protect a real account.
3. Set `apiRootCA` to the PEM root certificate(s) for the configured HTTPS servers. Use a C++ raw string, for example `R"PEM(...certificate PEM text...)PEM"`. HTTPS requests require a configured CA and a valid RTC time. There is no insecure TLS fallback.
4. Open `NanoInk.ino` in Arduino IDE, select ESP32 Dev Module and **Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**, then compile. This scheme targets a 4 MB flash device; confirm the actual flash capacity before uploading. The firmware exceeds the default 1.25 MB application partition. The CLI equivalent from this directory is:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

`config.local.h` is ignored by Git. Firmware binaries contain the configured secrets; build public demonstration binaries with test credentials.

## Service contracts

The backend is not included. Configure endpoints you control; the firmware does not require the original author's services.

The weather endpoint receives `location` and `key` query parameters and returns a QWeather-style JSON object, for example:

```json
{"code":"200","now":{"temp":"24","text":"晴","windDir":"东北风","windScale":"1-3","humidity":"55"}}
```

Required numeric fields must parse successfully; temperature must be between -100 and 100 °C, wind scale between 0 and 17, and humidity between 0 and 100%. Wind scale may also be an ordered range. Weather strings must fit the firmware buffers. Failed responses retain the previously displayed weather.

The timetable endpoint returns HTTP 200 with UTF-8 CSV, no header, ordered by start time:

```csv
1,2026-09-14 08:00:00,2026-09-14 09:40:00,嵌入式系统,A101
2,2026-09-14 10:00:00,2026-09-14 11:40:00,数字电路,B202
```

Fields are ID, start time, end time, course and location. Dates use `YYYY-MM-DD HH:MM:SS`, within the DS3231/RTClib 2000–2099 range. Fields may have surrounding double quotes, but embedded commas, quotes and newlines are not supported. End time must follow start time. An empty successful response represents an empty schedule. The sample dates are illustrative; choose dates appropriate to your clock for a live demonstration.

## Validation and current limits

The project uses established libraries for display driving, fonts, networking, clock access and TOTP. Its application logic integrates data synchronization, cached schedules, screen layout and sleep/wake behavior.

The original device is available, but Wi-Fi startup may currently cause a supply-voltage drop and repeated resets; this diagnosis needs reset logs and rail measurements. Brownout protection remains enabled. No current claims are made about battery life, whole-device sleep current, continuous-run stability or subsecond alignment to minute boundaries. The TOTP display refreshes at the screen's minute cadence, so it should not be treated as a continuously current authenticator display.

Before a hardware demonstration, verify stable power during Wi-Fi startup, cold boot, repeated sleep/wake display updates, offline operation, missing/expired schedules and the configured services. Record actual measurements separately from build or host-side logic checks.

## Credits

The project includes a GPLv3 license in `LICENSE`. The bundled bitmap fonts were generated from:

- **Vonwaon Bitmap 12 px and 16 px**, by Haoyu Qiu: [author's page](https://timothyqiu.itch.io/vonwaon-bitmap).
- **HarmonyOS Sans Medium**, copyright Huawei Device Co., Ltd.: [font information](https://developer.huawei.com/consumer/cn/doc/doccenter-ux-design/font-0000001828772001).

Third-party fonts retain their own terms.
