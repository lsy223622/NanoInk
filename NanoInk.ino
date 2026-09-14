#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>
#include <SPIFFS.h>
#include <TOTP.h>
#include <RTClib.h>
#include <sys/time.h>

#define ENABLE_GxEPD2_GFX 0
#define USE_HSPI_FOR_EPD
#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS GxEPD2_213_BN
#define GxEPD2_BW_IS_GxEPD2_BW true
#define IS_GxEPD(c, x) (c##x)
#define IS_GxEPD2_BW(x) IS_GxEPD(GxEPD2_BW_IS_, x)
#define MAX_DISPLAY_BUFFER_SIZE 65536ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))
GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(GxEPD2_DRIVER_CLASS(/*CS=*/15, /*DC=*/27, /*RST=*/26, /*BUSY=*/25));
SPIClass hspi(HSPI);

#include <U8g2_for_Adafruit_GFX.h>
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

#include "u8g2_fhpixelfont16px_12_gb2312.h"      // 字体
#include "u8g2_hmosfont48px_48_time.h"           // 字体
#include "u8g2_classperiod12px_9_classperiod.h"  // 字体

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);

DateTime now;
RTC_DS3231 rtc;

char batteryVoltageString[6] = "";  // 电池电压字符串

RTC_DATA_ATTR bool firstBoot = true;  // 第一次启动标志

RTC_DATA_ATTR char classStartTime[20] = "2023-01-01 00:00:00";  // 课程开始时间
RTC_DATA_ATTR char classEndTime[20] = "2023-01-01 00:00:00";    // 课程结束时间
RTC_DATA_ATTR char classCourse[40] = "没有获取到课程";          // 课程名
RTC_DATA_ATTR char classLocation[13] = "NULL";                  // 课程地点
RTC_DATA_ATTR char classPeriod[17] = "未知时间";                // 课程时间

RTC_DATA_ATTR int16_t qWeatherTemp = 0;             // 温度
RTC_DATA_ATTR char qWeatherText[25] = "无天气";     // 天气
RTC_DATA_ATTR char qWeatherWindDir[16] = "无风向";  // 风向
RTC_DATA_ATTR int16_t qWeatherWindScale = 0;        // 风力等级
RTC_DATA_ATTR int16_t qWeatherHumidity = 0;         // 湿度
RTC_DATA_ATTR char qWeatherString1[26] = "";        // 天气字符串
RTC_DATA_ATTR char qWeatherString2[26] = "";        // 风向字符串
RTC_DATA_ATTR char qWeatherString3[14] = "";        // 湿度字符串

RTC_DATA_ATTR bool serverConnected = false;  // 服务器连接状态

const char* WEEKDAY[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };  // 星期数组

#include "config.local.h"

const int WIFI_RETRY_LIMIT = 5;
const int HOTSPOT_RETRY_LIMIT = 10;
const int WIFI_TIMEOUT = 1000;
TOTP totp(hmacKey, sizeof(hmacKey));

WiFiClientSecure* getSecureClient() {
  static WiFiClientSecure client;  // 静态对象复用
  if (apiRootCA[0] == '\0') {
    Serial.println("Configure apiRootCA before using HTTPS.");
    return nullptr;
  }
  DateTime localTime = rtc.now();
  if (rtc.lostPower() || !localTime.isValid()) return nullptr;
  // The DS3231 stores UTC+8; TLS uses the system clock in UTC.
  timeval utc = { static_cast<time_t>(localTime.unixtime() - 8 * 3600), 0 };
  settimeofday(&utc, nullptr);
  client.setCACert(apiRootCA);
  client.setTimeout(10);  // 设置10秒超时
  return &client;
}

void setup() {
  Serial.begin(115200);   // 初始化串口
  print_wakeup_reason();  // 打印睡眠唤醒原因

  // 记录是否从深度睡眠唤醒
  const esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  const bool wokeFromDeepSleep = (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED);

// 初始化屏幕
#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
  hspi.begin(13, 12, 14, 15);  // remap hspi for EPD (swap pins)
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif

  // 初始化SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");
    return;
  }

  // 初始化RTC
  if (!rtc.begin()) {
    Serial.println("RTC initialization failed!");
    while (1) {};
  }

  // 只有第一次启动会触发，清屏联网同步数据
  if (firstBoot) {
    firstBoot = false;                // 设置第一次启动标志为false
    Serial.println("Initial boot.");  // 打印日志

    // 初始化屏幕
    display.init(115200);                       // 初始化屏幕
    display.setRotation(1);                     // 设置屏幕旋转方向，分别有0，1，2，3这四个方向
    display.setTextWrap(false);                 // 设置文本是否自动换行，false则为不自动换行，如果文本溢出则显示异常或者不显示
    display.setTextColor(GxEPD_BLACK);          // 设置 文本颜色
    u8g2Fonts.begin(display);                   // 将u8g2过程连接到Adafruit GFX
    u8g2Fonts.setFontMode(1);                   // 使用u8g2透明模式（这是默认设置）
    u8g2Fonts.setFontDirection(0);              // 从左到右（这是默认设置）
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);  // 设置前景色
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);  // 设置背景色

    display.setFullWindow();                                     // 设置刷新模式为全屏刷新
    u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);           // 设置字体
    uint16_t connW = u8g2Fonts.getUTF8Width("Initializing...");  // 计算字符串宽度

    // 显示初始化页面
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      u8g2Fonts.setCursor(125 - connW / 2, 62);
      u8g2Fonts.print("Initializing...");
    } while (display.nextPage());

    if (connectWifi() < 3) {
      displaySyncingBadge();   // 显示同步标志
      syncTime();              // 同步时间
      updateCurrentWeather();  // 更新天气
      updateClassTimeTable();  // 更新课程表
    }
  }

  // 若为深度睡眠唤醒，强制进行显示控制器初始化，避免局部刷新无效
  display.init(115200, wokeFromDeepSleep, 10, false);  // 初始化屏幕
  display.setRotation(1);                     // 设置屏幕旋转方向，分别有0，1，2，3这四个方向
  display.setTextWrap(false);                 // 设置文本是否自动换行，false则为不自动换行，如果文本溢出则显示异常或者不显示
  display.setTextColor(GxEPD_BLACK);          // 设置 文本颜色
  u8g2Fonts.begin(display);                   // 将u8g2过程连接到Adafruit GFX
  u8g2Fonts.setFontMode(1);                   // 使用u8g2透明模式（这是默认设置）
  u8g2Fonts.setFontDirection(0);              // 从左到右（这是默认设置）
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);  // 设置前景色
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);  // 设置背景色

  DateTime temp = rtc.now();
  now = std::move(temp);  // 更新时间

  updateClass();

  mainDisplay(now.minute() % 30 != 0);  // 更新屏幕

  // 每小时联网同步数据
  if (now.minute() == 0 && connectWifi() < 3) {
    displaySyncingBadge();  // 每小时联网同步时间
    DateTime temp = rtc.now();
    now = std::move(temp);                                                                         // 更新时间
    if (rtc.lostPower() || !now.isValid() || now.hour() == 12) syncTime();
    if (now.dayOfTheWeek() == 0 && now.hour() == 12 && now.minute() == 0) {
      updateClassTimeTable();
      updateClass();
    }
    updateCurrentWeather();                                                                        // 每小时联网同步天气
    mainDisplay(true);                                                                             // 更新屏幕
  }

  display.hibernate();          // 屏幕进入休眠模式
  deepSleep2NextWholeMinute();  // 进入深度睡眠模式
}

void loop() {
  // do nothing, only setup runs
}

void mainDisplay(bool partial) {
  Serial.println("Display main page.");  // 打印日志

  updateBatteryVoltage();  // 更新电池电压

  DateTime temp = rtc.now();
  now = std::move(temp);                                                                                        // 更新时间
  char currentDateString[27];                                                                                   // 日期格式：2021-01-01 星期一
  snprintf(currentDateString, sizeof(currentDateString), "%4d-%d-%d %s", now.year(), now.month(), now.day(), WEEKDAY[now.dayOfTheWeek()]);  // 生成日期字符串
  char currentTimeString[8];                                                                                    // 时间格式：00:00
  snprintf(currentTimeString, sizeof(currentTimeString), "%02d:%02d", now.hour(), now.minute());                                            // 生成时间字符串

  char totpCode[7] = "";                                      // TOTP动态验证码
  if (!rtc.lostPower() && now.isValid()) {
    snprintf(totpCode, sizeof(totpCode), "%s", totp.getCode(now.unixtime() - 8 * 3600));
  }

  u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);           // 设置字体
  uint16_t dateW = u8g2Fonts.getUTF8Width(currentDateString);  // 计算日期字符串宽度
  uint16_t locationW = u8g2Fonts.getUTF8Width(classLocation);  // 计算地点字符串宽度

  // 设置刷新模式
  if (partial) {
    display.setPartialWindow(0, 0, 250, 128);
  } else {
    display.setFullWindow();
  }

  // 主页面布局
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFont(u8g2_hmosfont48px_48_time);
    u8g2Fonts.setCursor(83, 101);
    u8g2Fonts.print(currentTimeString);
    u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);
    u8g2Fonts.setCursor(2, 15);
    u8g2Fonts.print(qWeatherString1);
    u8g2Fonts.setCursor(2, 33);
    u8g2Fonts.print(qWeatherString2);
    u8g2Fonts.setCursor(2, 51);
    u8g2Fonts.print(qWeatherString3);
    u8g2Fonts.setCursor(2, 87);
    u8g2Fonts.print(totpCode);

    // Display badge
    display.fillRect(144, 1, 106, 14, GxEPD_WHITE);
    u8g2Fonts.setCursor(146, 14);
    u8g2Fonts.print("Server");
    display.drawRoundRect(144, 1, 106, 14, 3, GxEPD_BLACK);
    display.fillRect(194, 1, 14, 14, GxEPD_BLACK);
    if (serverConnected) {
      display.drawLine(196, 8, 199, 11, GxEPD_WHITE);
      display.drawLine(200, 10, 205, 5, GxEPD_WHITE);
    } else {
      display.drawLine(197, 4, 204, 11, GxEPD_WHITE);
      display.drawLine(197, 11, 204, 4, GxEPD_WHITE);
    }
    u8g2Fonts.setCursor(209, 14);
    u8g2Fonts.print(batteryVoltageString);

    u8g2Fonts.setCursor(249 - dateW, 46);
    u8g2Fonts.print(currentDateString);
    u8g2Fonts.setCursor(2, 120);
    u8g2Fonts.print(classCourse);
    display.fillRect(249 - locationW, 105, locationW + 1, 17, GxEPD_WHITE);
    u8g2Fonts.setCursor(250 - locationW, 120);
    u8g2Fonts.print(classLocation);
    display.drawLine(250 - locationW, 121, 249, 121, GxEPD_BLACK);
    u8g2Fonts.setFont(u8g2_classperiod12px_9_classperiod);
    u8g2Fonts.setCursor(2, 102);
    u8g2Fonts.print(classPeriod);
  } while (display.nextPage());
}

// 显示同步标志
void displaySyncingBadge() {
  updateBatteryVoltage();                             // 更新电池电压
  Serial.println("Display syncing badge.");           // 打印日志
  u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);  // 设置字体
  display.setPartialWindow(143, 0, 109, 18);          // 设置局部刷新区域
  display.firstPage();
  do {
    display.fillRect(143, 0, 109, 18, GxEPD_WHITE);
    u8g2Fonts.setCursor(146, 14);
    u8g2Fonts.print("SYNCING");
    display.drawRoundRect(144, 1, 106, 14, 3, GxEPD_BLACK);
    display.fillRect(202, 1, 6, 14, GxEPD_BLACK);
    u8g2Fonts.setCursor(209, 14);
    u8g2Fonts.print(batteryVoltageString);
  } while (display.nextPage());
}

// 连接WiFi
int connectWifi() {
  WiFi.mode(WIFI_STA);  // 显式设置WiFi模式

  Serial.println("Connecting to WiFi...");  // 打印日志

  // 如果已经连接WiFi，则直接返回
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi already connected.");
    return 2;
  }

  // 尝试连接主WiFi
  WiFi.begin(wifiSSID, wifiPass);
  for (int retry = 0; retry < WIFI_RETRY_LIMIT; retry++) {
    Serial.println("Connecting to WiFi...");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to WiFi!");
      return 1;
    }
    delay(WIFI_TIMEOUT);
  }

  // 尝试连接热点
  WiFi.disconnect();  // 断开之前的连接
  WiFi.begin(hotspotSSID, hotspotPass);
  for (int retry = 0; retry < HOTSPOT_RETRY_LIMIT; retry++) {
    Serial.println("Connecting to mobile hotspot...");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to mobile hotspot!");
      return 1;
    }
    delay(WIFI_TIMEOUT);
  }

  WiFi.disconnect(true);  // 清理WiFi配置
  Serial.println("Failed to connect to WiFi.");
  return 3;
}

// 同步时间
void syncTime() {
  Serial.println("Syncing time...");
  timeClient.begin();
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (timeClient.forceUpdate()) {
      DateTime synced(timeClient.getEpochTime());
      if (synced.isValid()) {
        rtc.adjust(synced);
        timeClient.end();
        Serial.println("DS3231 time updated from NTP");
        return;
      }
    }
  }
  timeClient.end();
  Serial.println("Failed to sync time; keeping RTC time.");
}

bool parseClassLine(String line, String fields[]) {
  line.trim();
  int begin = 0;
  for (int i = 0; i < 5; ++i) {
    int end = line.indexOf(',', begin);
    if ((i < 4 && end < 0) || (i == 4 && end >= 0)) return false;
    fields[i] = i < 4 ? line.substring(begin, end) : line.substring(begin);
    fields[i].trim();
    if (fields[i].startsWith("\"") && fields[i].endsWith("\"") && fields[i].length() >= 2) {
      fields[i] = fields[i].substring(1, fields[i].length() - 1);
    }
    if (fields[i].isEmpty() || fields[i].indexOf('"') >= 0) return false;
    begin = end + 1;
  }
  time_t start = getTimeStamp(fields[1].c_str());
  time_t end = getTimeStamp(fields[2].c_str());
  return start != 0 && end > start;
}

// Read the first unexpired record without modifying the cached timetable.
void updateClass() {
  classStartTime[0] = '\0';
  classEndTime[0] = '\0';
  snprintf(classCourse, sizeof(classCourse), "暂无课程");
  classLocation[0] = '\0';
  classPeriod[0] = '\0';
  now = rtc.now();
  if (rtc.lostPower() || !now.isValid()) return;
  File file = SPIFFS.open("/classtimetable.csv", FILE_READ);
  if (!file) file = SPIFFS.open("/classtimetable.bak", FILE_READ);
  if (!file) return;

  while (file.available()) {
    String fields[5];
    if (!parseClassLine(file.readStringUntil('\n'), fields)) continue;
    time_t start = getTimeStamp(fields[1].c_str());
    time_t end = getTimeStamp(fields[2].c_str());
    if (end <= now.unixtime()) continue;
    snprintf(classStartTime, sizeof(classStartTime), "%s", fields[1].c_str());
    snprintf(classEndTime, sizeof(classEndTime), "%s", fields[2].c_str());
    String course = subStringX(fields[3], 13, sizeof(classCourse) - 1);
    if (course != fields[3]) course = subStringX(fields[3], 12, sizeof(classCourse) - 4) + "…";
    snprintf(classCourse, sizeof(classCourse), "%s", course.c_str());
    snprintf(classLocation, sizeof(classLocation), "%s",
             subStringX(fields[4], 7, sizeof(classLocation) - 1).c_str());
    if (start > now.unixtime()) {
      snprintf(classPeriod, sizeof(classPeriod), "下一节: %.5s", classStartTime + 11);
    } else {
      snprintf(classPeriod, sizeof(classPeriod), "%.5s - %.5s", classStartTime + 11, classEndTime + 11);
    }
    break;
  }
  file.close();
}

// 更新天气
void updateCurrentWeather() {
  serverConnected = false;
  WiFiClientSecure* client = getSecureClient();
  if (!client) return;
  HTTPClient http;
  String requestUrl = String(qWeatherURL) + "?location=" + qWeatherLocation + "&key=" + qWeatherKey;
  if (!requestUrl.startsWith("https://") || !http.begin(*client, requestUrl)) return;
  http.setTimeout(10000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    if (!error && doc["code"].as<String>() == "200") {
      JsonObject weather = doc["now"];
      int temp, scale, humidity, scaleUpper;
      String scaleText = weather["windScale"].as<String>();
      int dash = scaleText.indexOf('-');
      bool validScale = scaleText.length() <= 5 && parseWeatherNumber(dash < 0 ? scaleText : scaleText.substring(0, dash), 0, 17, scale);
      if (dash >= 0) {
        validScale = validScale && parseWeatherNumber(scaleText.substring(dash + 1), scale, 17, scaleUpper);
      }
      const char* text = weather["text"];
      const char* direction = weather["windDir"];
      if (text && direction && strlen(text) < sizeof(qWeatherText) &&
          strlen(direction) < sizeof(qWeatherWindDir) &&
          parseWeatherNumber(weather["temp"].as<String>(), -100, 100, temp) &&
          validScale &&
          parseWeatherNumber(weather["humidity"].as<String>(), 0, 100, humidity)) {
        qWeatherTemp = temp;
        qWeatherWindScale = scale;
        qWeatherHumidity = humidity;
        snprintf(qWeatherText, sizeof(qWeatherText), "%s", text);
        snprintf(qWeatherWindDir, sizeof(qWeatherWindDir), "%s", direction);
        char windScale[10] = "";
        if (scale != 0 || dash >= 0) snprintf(windScale, sizeof(windScale), "%s级", scaleText.c_str());
        snprintf(qWeatherString1, sizeof(qWeatherString1), "%s%d℃",
                 subStringX(String(text), 2, 6).c_str(), temp);
        snprintf(qWeatherString2, sizeof(qWeatherString2), "%s%s", direction, windScale);
        snprintf(qWeatherString3, sizeof(qWeatherString3), "湿度%d%%", humidity);
        serverConnected = true;
      }
    }
  } else {
    Serial.printf("Weather HTTP status: %d\n", httpCode);
  }
  http.end();
}

bool parseWeatherNumber(String text, int minimum, int maximum, int& result) {
  char* end = nullptr;
  long number = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || number < minimum || number > maximum) return false;
  result = static_cast<int>(number);
  return true;
}

void updateClassTimeTable() {
  WiFiClientSecure* client = getSecureClient();
  if (!client) return;
  HTTPClient http;
  if (!String(classTimeAPI).startsWith("https://") || !http.begin(*client, classTimeAPI)) return;
  http.setTimeout(10000);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Timetable HTTP status: %d\n", httpCode);
    http.end();
    return;
  }
  const char* temporaryPath = "/classtimetable.tmp";
  File file = SPIFFS.open(temporaryPath, FILE_WRITE);
  if (!file) { http.end(); return; }
  int expected = http.getSize();
  int written = http.writeToStream(&file);
  bool complete = written >= 0 && (expected < 0 || written == expected) &&
                  static_cast<size_t>(written) == file.size();
  file.close();
  http.end();
  file = SPIFFS.open(temporaryPath, FILE_READ);
  bool valid = complete && static_cast<bool>(file);
  time_t previousStart = 0;
  while (valid && file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    String fields[5];
    valid = parseClassLine(line, fields);
    if (valid) {
      time_t start = getTimeStamp(fields[1].c_str());
      valid = start >= previousStart;
      previousStart = start;
    }
  }
  file.close();
  const char* cachedPath = "/classtimetable.csv";
  const char* backupPath = "/classtimetable.bak";
  bool hadCache = SPIFFS.exists(cachedPath);
  // SPIFFS cannot rename onto an existing file. Keep the old cache until promotion succeeds.
  if (valid && hadCache) {
    if (SPIFFS.exists(backupPath)) valid = SPIFFS.remove(backupPath);
    if (valid) valid = SPIFFS.rename(cachedPath, backupPath);
  }
  if (valid && SPIFFS.rename(temporaryPath, cachedPath)) {
    SPIFFS.remove(backupPath);
    Serial.println("Timetable updated.");
    return;
  }
  if (hadCache && !SPIFFS.exists(cachedPath)) SPIFFS.rename(backupPath, cachedPath);
  SPIFFS.remove(temporaryPath);
  Serial.println("Timetable update failed; keeping cached data.");
}

// Parse local wall time with RTClib, matching the UTC+8 DS3231 convention.
time_t getTimeStamp(const char* text) {
  if (strlen(text) != 19) return 0;
  for (int i = 0; i < 19; ++i) {
    char separator = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : '\0';
    if (separator ? text[i] != separator : text[i] < '0' || text[i] > '9') return 0;
  }
  int year, month, day, hour, minute, second;
  if (sscanf(text, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6 ||
      year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 59) return 0;
  DateTime parsed(year, month, day, hour, minute, second);
  return parsed.isValid() ? parsed.unixtime() : 0;
}

String subStringX(String str, int length, size_t maxBytes) {
  size_t end = 0;
  int count = 0;
  while (end < str.length() && count < length) {
    uint8_t lead = static_cast<uint8_t>(str[end]);
    size_t bytes = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 :
                   (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (bytes == 0 || end + bytes > str.length() || end + bytes > maxBytes) break;
    for (size_t i = 1; i < bytes; ++i) {
      if ((static_cast<uint8_t>(str[end + i]) & 0xC0) != 0x80) return str.substring(0, end);
    }
    end += bytes;
    ++count;
  }
  return str.substring(0, end);
}

// 打印睡眠唤醒原因
void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;        // 声明变量
  wakeup_reason = esp_sleep_get_wakeup_cause();  // 获取睡眠唤醒原因

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("Wakeup caused by external signal using RTC_IO"); break;    // 外部中断引脚唤醒
    case ESP_SLEEP_WAKEUP_EXT1: Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;  // 外部中断引脚唤醒
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Wakeup caused by timer"); break;                          // 定时器唤醒
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Wakeup caused by touchpad"); break;                    // 触摸唤醒
    case ESP_SLEEP_WAKEUP_ULP: Serial.println("Wakeup caused by ULP program"); break;                      // ULP唤醒
    default: Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason); break;             // 其他原因唤醒
  }
}

// 进入深度睡眠模式，直到下一个整点
void deepSleep2NextWholeMinute() {
  DateTime temp = rtc.now();
  now = std::move(temp);

  // 添加安全检查
  if (now.second() >= 60) {
    Serial.println("Invalid RTC time!");
    esp_restart();  // 重启设备
    return;
  }

  uint64_t sleepTime = (60 - now.second()) * 1000ULL;
  sleepTime += (1000ULL - (esp_timer_get_time() % 1000ULL));

  // 添加合理范围检查
  if (sleepTime > 61000ULL || sleepTime < 1000ULL) {
    Serial.printf("Invalid sleep time: %llu ms\n", sleepTime);
    sleepTime = 60000ULL;  // 使用默认值
  }

  // 清理资源
  WiFi.disconnect(true);
  display.hibernate();

  esp_sleep_enable_timer_wakeup(sleepTime * 1000ULL);
  esp_deep_sleep_start();
}

// 更新电池电压
void updateBatteryVoltage() {
  int voltageADC = analogRead(36);                               // 读取ADC值
  float batteryVoltageRead = voltageADC * 1.1 / 455;             // 将读取到的ADC值转换为电压值
  Serial.print("Battery voltage: ");                             // 打印电压值
  char batteryVoltagePrecise[8];                                 // 定义电压值字符串
  snprintf(batteryVoltagePrecise, sizeof(batteryVoltagePrecise), "%5.4fV", batteryVoltageRead);  // 将电压值转换为字符串
  Serial.println(String(batteryVoltagePrecise));                 // 打印电压值
  snprintf(batteryVoltageString, sizeof(batteryVoltageString), "%3.2fV", batteryVoltageRead);   // 将电压值转换为字符串
}

// 解析日期时间字符串
DateTime parseDateTime(const char* dateTimeStr) {
  int year, month, day, hour, minute, second;                                              // 定义变量
  sscanf(dateTimeStr, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);  // 解析日期时间字符串
  return DateTime(year, month, day, hour, minute, second);                                 // 返回DateTime对象
}

// 建议添加文件系统检查和错误恢复机制
bool initFileSystem() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");

    // 尝试格式化并重新挂载
    if (SPIFFS.format()) {
      if (SPIFFS.begin(true)) {
        Serial.println("SPIFFS formatted and mounted successfully");
        return true;
      }
    }
    return false;
  }
  return true;
}
