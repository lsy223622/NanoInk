# NanoInk

[English version](README.md)

NanoInk 是一个个人 ESP32 电子纸信息终端，用于显示日常课程表、天气、时间、电池电压和 TOTP 动态验证码。它结合了 DS3231 时钟、本地缓存的课程表和周期性深度睡眠。

## 硬件与运行方式

Arduino IDE 中的开发板选项为 **ESP32 Dev Module**（`esp32:esp32:esp32`）。显示驱动使用 GxEPD2 `GxEPD2_213_BN`，绘图区旋转后为 250 × 128。连接之前请确认实际电子纸面板与该驱动匹配。

| 连接 | ESP32 GPIO |
| --- | --- |
| 电子纸 SCK / MISO / MOSI | 13 / 12 / 14 |
| 电子纸 CS / DC / RST / BUSY | 15 / 27 / 26 / 25 |
| DS3231 I²C | 开发板默认 SDA / SCL |
| 电池测量输入 | 36 |

当前电池电压换算使用 `ADC × 1.1 / 455`。分压电路、供电电路和校准方式取决于原始硬件，目前没有完成记录或重新测量。没有确认电路和输入上限之前，不要将电池电压直接接到 ADC。

首次启动时，设备会尝试连接 Wi-Fi，失败后切换到配置的手机热点，同步时间并下载数据。之后设备大约每分钟唤醒一次；除半小时边界外使用局部刷新，并在每个整点尝试联网更新。NTP 校时安排在每天中午，课程表在每周日中午刷新。如果 RTC 报告掉电，设备也会在整点联网时尝试校时。

DS3231 和课程时间戳使用 **UTC+8 本地墙上时间**。TOTP 和 TLS 系统时钟使用 UTC。固件从缓存中读取第一条尚未结束的课程；没有课程或课程表已耗尽时显示无课程。下载失败或内容无效时保留之前的课程表。由于 SPIFFS 不能将文件重命名覆盖到已有目标，更新过程使用临时文件和备份文件。这个过程尚未在实机上验证掉电安全性。首次挂载 SPIFFS 时仍保留 Arduino 在挂载失败后自动格式化的行为。

## 构建与配置

使用下面的 Boards Manager 地址安装 ESP32 开发板包：

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

使用 ESP32 Core **3.3.5**，以及下面列出的 Library Manager 库版本：

| 库 | 版本 |
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

1. 将 `config.example.h` 复制为草图目录中的 `config.local.h`。
2. 填写网络设置、API 地址、天气服务的 key/location 和 TOTP 种子。示例 TOTP 种子是公开测试向量，不能用于保护真实账户。
3. 将配置的 HTTPS 服务所使用的 PEM 根证书填入 `apiRootCA`。可以使用 C++ 原始字符串，例如 `R"PEM(...certificate PEM text...)PEM"`。HTTPS 请求要求已配置 CA，并且 RTC 时间有效；固件没有不安全 TLS 回退路径。
4. 在 Arduino IDE 中打开 `NanoInk.ino`，选择 ESP32 Dev Module 和 **Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**，然后编译。该分区方案面向 4 MB Flash 设备；上传前请确认实际 Flash 容量。当前固件超过默认的 1.25 MB 应用分区。此目录下使用 CLI 的等价命令为：

```sh
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app .
```

`config.local.h` 已被 Git 忽略。固件二进制文件会包含配置的秘密信息；用于公开演示的固件应使用测试凭据。

## 服务接口约定

仓库不包含后端服务。请配置由你控制的接口；固件不依赖原作者的服务。

天气接口接收 `location` 和 `key` 查询参数，并返回类似 QWeather 的 JSON 对象，例如：

```json
{"code":"200","now":{"temp":"24","text":"晴","windDir":"东北风","windScale":"1-3","humidity":"55"}}
```

数值字段必须能够成功解析；温度必须在 -100 到 100 °C 之间，风力等级必须在 0 到 17 之间，湿度必须在 0% 到 100% 之间。风力等级也可以使用有序范围。天气文字必须能够放入固件缓冲区。请求失败时保留之前显示的天气数据。

课程表接口返回 HTTP 200 和 UTF-8 编码的 CSV，不包含表头，并按开始时间排序：

```csv
1,2026-09-14 08:00:00,2026-09-14 09:40:00,嵌入式系统,A101
2,2026-09-14 10:00:00,2026-09-14 11:40:00,数字电路,B202
```

字段依次为课程 ID、开始时间、结束时间、课程名称和地点。日期使用 `YYYY-MM-DD HH:MM:SS` 格式，并且必须处于 DS3231/RTClib 支持的 2000–2099 年范围内。字段可以带有首尾双引号，但不支持字段内部的逗号、双引号和换行。结束时间必须晚于开始时间。成功返回空内容表示空课程表。示例日期仅用于说明；实际演示时请根据设备时钟选择合适的日期。

## 验证与当前限制

项目使用成熟的显示驱动、字体、网络、时钟访问和 TOTP 库。应用层逻辑负责整合数据同步、课程表缓存、屏幕布局以及休眠/唤醒行为。

原始设备仍在，但 Wi-Fi 启动时可能出现供电电压下降并反复重启；这个判断还需要复位原因日志和电源轨测量来确认。欠压保护保持启用。目前不对电池续航、整机休眠电流、连续运行稳定性或分钟边界的亚秒级对齐做出声明。TOTP 显示按屏幕的分钟级刷新节奏更新，因此不能将它当作持续保持最新的身份验证器显示。

在进行实机演示之前，请验证 Wi-Fi 启动、冷启动、反复休眠/唤醒并刷新电子纸、离线运行、课程表缺失或过期，以及配置服务不可用时的行为。请将实际测量结果与构建检查或主机逻辑检查分开记录。

## 致谢与来源

项目包含 `LICENSE` 中的 GPLv3 许可证。仓库中的位图字体由以下字体生成：

- **Vonwaon Bitmap 12 px 和 16 px**，作者为 Haoyu Qiu：[作者页面](https://timothyqiu.itch.io/vonwaon-bitmap)。
- **HarmonyOS Sans Medium**，版权归 Huawei Device Co., Ltd. 所有：[字体信息](https://developer.huawei.com/consumer/cn/doc/doccenter-ux-design/font-0000001828772001)。

第三方字体仍适用其各自的授权条款。
