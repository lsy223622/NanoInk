#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>
#include <time.h>
#include <SPIFFS.h>

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

#include "u8g2_fhpixelfont16px_12_gb2312.h"
#include "u8g2_hmosfont48px_48_time.h"
#include "u8g2_classperiod12px_9_classperiod.h"

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600);

RTC_DATA_ATTR int bootCycle = 0;

RTC_DATA_ATTR char classStartTime[20] = "2023-01-01 00:00:00";
RTC_DATA_ATTR char classEndTime[20] = "2023-01-01 00:00:00";
RTC_DATA_ATTR char classCourse[40] = "没有获取到课程";
RTC_DATA_ATTR char classLocation[13] = "NULL";
RTC_DATA_ATTR char classPeriod[17] = "未知时间";

RTC_DATA_ATTR int16_t qWeatherTemp = 0;
RTC_DATA_ATTR char qWeatherText[25] = "无天气";
RTC_DATA_ATTR char qWeatherWindDir[16] = "无风向";
RTC_DATA_ATTR int16_t qWeatherWindScale = 0;
RTC_DATA_ATTR int16_t qWeatherHumidity = 0;
RTC_DATA_ATTR char qWeatherString1[26] = "";
RTC_DATA_ATTR char qWeatherString2[11] = "";
RTC_DATA_ATTR char qWeatherString3[11] = "";

RTC_DATA_ATTR bool serverConnected = false;

const char* WEEKDAY[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };

// 自定义设置
const char wifiSSID[] = "REDACTED";
const char wifiPass[] = "REDACTED";
const char hotspotSSID[] = "REDACTED";
const char hotspotPass[] = "REDACTED";
const char qWeatherURL[] = "https://api.example.com/proxyforqweather.php";
const char qWeatherKey[] = "REDACTED";
const char qWeatherLocation[] = "000000000";
const char classTimeAPI[] = "https://api.example.com/class_time.php";

void setup() {
  Serial.begin(115200);

  bootCycle++;
  Serial.println("bootCycle:" + String(bootCycle));

  print_wakeup_reason();

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
  hspi.begin(13, 12, 14, 15);  // remap hspi for EPD (swap pins)
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif

  time_t now;
  struct tm timeinfo;

  // 只有第一次启动会触发，清屏联网同步时间
  if (bootCycle == 1) {
    Serial.println("Initial boot.");

    display.init(115200);                       // 初始化屏幕
    display.setRotation(1);                     // 设置屏幕旋转方向，分别有0，1，2，3这四个方向
    display.setTextWrap(false);                 // 设置文本是否自动换行，false则为不自动换行，如果文本溢出则显示异常或者不显示
    display.setTextColor(GxEPD_BLACK);          // 设置 文本颜色
    u8g2Fonts.begin(display);                   // 将u8g2过程连接到Adafruit GFX
    u8g2Fonts.setFontMode(1);                   // 使用u8g2透明模式（这是默认设置）
    u8g2Fonts.setFontDirection(0);              // 从左到右（这是默认设置）
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);  // 设置前景色
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);  // 设置背景色

    display.setFullWindow();
    u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);
    uint16_t connw = u8g2Fonts.getUTF8Width("Initializing...");

    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      u8g2Fonts.setCursor(125 - connw / 2, 62);
      u8g2Fonts.print("Initializing...");
    } while (display.nextPage());

    displaySyncingBadge();

    connectAndSyncTime();
    connectAndUpdateClass();
    connectAndUpdateCurrentWeather();
    connectAndUpdateClassTimeTable();

    time(&now);
    localtime_r(&now, &timeinfo);

    char currentDateString[18];
    sprintf(currentDateString, "%4d-%d-%d %s", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, WEEKDAY[timeinfo.tm_wday]);
    char currentTimeString[6];
    sprintf(currentTimeString, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    mainDisplay(currentDateString, currentTimeString, false);

    display.hibernate();

    deepSleep2NextWholeMinute();
  }

  display.init(115200, false, 10, false);     // 初始化屏幕
  display.setRotation(1);                     // 设置屏幕旋转方向，分别有0，1，2，3这四个方向
  display.setTextWrap(false);                 // 设置文本是否自动换行，false则为不自动换行，如果文本溢出则显示异常或者不显示
  display.setTextColor(GxEPD_BLACK);          // 设置 文本颜色
  u8g2Fonts.begin(display);                   // 将u8g2过程连接到Adafruit GFX
  u8g2Fonts.setFontMode(1);                   // 使用u8g2透明模式（这是默认设置）
  u8g2Fonts.setFontDirection(0);              // 从左到右（这是默认设置）
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);  // 设置前景色
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);  // 设置背景色

  // if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
  //   Serial.println("ESP_SLEEP_WAKEUP_EXT1");

  //   u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);
  //   display.setPartialWindow(186, 1, 64, 14);
  //   display.firstPage();
  //   do {
  //     display.fillRect(186, 1, 64, 14, GxEPD_WHITE);
  //     display.fillRoundRect(186, 1, 64, 14, 3, GxEPD_BLACK);
  //     display.fillRoundRect(187, 2, 56, 12, 2, GxEPD_WHITE);
  //     u8g2Fonts.setCursor(188, 14);
  //     u8g2Fonts.print("SYNCING");
  //   } while (display.nextPage());

  //   connectAndSyncTime();
  //   connectAndUpdateCurrentWeather();
  //   connectAndUpdateClass();

  //   time(&now);
  //   localtime_r(&now, &timeinfo);

  //   char currentDateString[18];
  //   sprintf(currentDateString, "%4d-%d-%d %s", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, WEEKDAY[timeinfo.tm_wday]);
  //   char currentTimeString[6];
  //   sprintf(currentTimeString, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  //   mainDisplay(currentDateString, currentTimeString, false);

  //   display.hibernate();

  //   // gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);
  //   // gpio_wakeup_enable(GPIO_NUM_12, GPIO_INTR_HIGH_LEVEL);
  //   // esp_sleep_enable_gpio_wakeup();

  //   // // 配置IO12引脚为输入模式，并设置内部上拉电阻
  //   // gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);
  //   // gpio_pullup_en(GPIO_NUM_12);

  //   pinMode(12, INPUT_PULLUP);
  //   // 配置EXT1唤醒方式
  //   esp_sleep_enable_ext1_wakeup(GPIO_SEL_12, ESP_EXT1_WAKEUP_ANY_HIGH);

  //   // esp_sleep_enable_ext0_wakeup(GPIO_NUM_12, 1);

  //   esp_sleep_enable_timer_wakeup(sleepTime * 1000ULL);

  //   esp_deep_sleep_start();
  // }

  time(&now);
  localtime_r(&now, &timeinfo);

  char currentDateString[18];
  sprintf(currentDateString, "%4d-%d-%d %s", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, WEEKDAY[timeinfo.tm_wday]);
  char currentTimeString[6];
  sprintf(currentTimeString, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  mainDisplay(currentDateString, currentTimeString, timeinfo.tm_min % 30 != 0);

  // 每个整20分钟联网同步时间和天气
  if (bootCycle == 2 || timeinfo.tm_min % 20 == 0) {
    displaySyncingBadge();
    connectAndSyncTime();
    connectAndUpdateCurrentWeather();
    mainDisplay(currentDateString, currentTimeString, true);
  }

  // 每个20分钟前标记同步时间和天气
  if (timeinfo.tm_min % 20 == 19) bootCycle = 1;

  if (timeinfo.tm_wday == 0 && timeinfo.tm_hour == 20 && timeinfo.tm_min == 0 && timeinfo.tm_sec == 0) {
    connectAndUpdateClassTimeTable();
  }

  time(&now);
  if (getTimeStamp(classEndTime) <= now && timeinfo.tm_min % 5 == 0) {
    displaySyncingBadge();
    connectAndUpdateClass();
    mainDisplay(currentDateString, currentTimeString, true);
  }

  display.hibernate();

  // // gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);
  // // gpio_wakeup_enable(GPIO_NUM_12, GPIO_INTR_HIGH_LEVEL);
  // // esp_sleep_enable_gpio_wakeup();

  // // // 配置IO12引脚为输入模式，并设置内部上拉电阻
  // // gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);
  // // gpio_pullup_en(GPIO_NUM_12);

  // // pinMode(12, INPUT_PULLUP);
  // // // 配置EXT1唤醒方式
  // // esp_sleep_enable_ext1_wakeup(GPIO_SEL_12, ESP_EXT1_WAKEUP_ANY_HIGH);

  // // esp_sleep_enable_ext0_wakeup(GPIO_NUM_12, 1);

  // esp_sleep_enable_timer_wakeup(sleepTime * 1000ULL);
  // esp_deep_sleep_start();

  deepSleep2NextWholeMinute();
}

void loop() {
  // do nothing, only setup runs
}

void mainDisplay(char* currentDateString, char* currentTimeString, bool partial) {
  int voltageADC = analogRead(36);
  float batteryVoltage = voltageADC * 1.1 / 455;  //将读取到的ADC值转换为电压值
  Serial.print("Battery voltage: ");
  char batteryVoltagePrecise[8];
  sprintf(batteryVoltagePrecise, "%5.4fV", batteryVoltage);
  Serial.println(String(batteryVoltagePrecise));
  char batteryVoltageString[6];
  sprintf(batteryVoltageString, "%3.2fV", batteryVoltage);

  u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);
  uint16_t datew = u8g2Fonts.getUTF8Width(currentDateString);
  uint16_t locationw = u8g2Fonts.getUTF8Width(classLocation);
  Serial.println("Display main page.");

  if (partial) {
    display.setPartialWindow(0, 0, 250, 128);
  } else {
    display.setFullWindow();
  }

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
    // Finish displaying badge
    u8g2Fonts.setCursor(249 - datew, 46);
    u8g2Fonts.print(currentDateString);
    u8g2Fonts.setCursor(2, 120);
    u8g2Fonts.print(classCourse);
    display.fillRect(249 - locationw, 105, locationw + 1, 17, GxEPD_WHITE);
    u8g2Fonts.setCursor(250 - locationw, 120);
    u8g2Fonts.print(classLocation);
    display.drawLine(250 - locationw, 121, 249, 121, GxEPD_BLACK);
    u8g2Fonts.setFont(u8g2_classperiod12px_9_classperiod);
    u8g2Fonts.setCursor(2, 102);
    u8g2Fonts.print(classPeriod);
  } while (display.nextPage());
}

void displaySyncingBadge() {
  int voltageADC = analogRead(36);
  float batteryVoltage = voltageADC * 1.1 / 455;  //将读取到的ADC值转换为电压值
  char batteryVoltageString[6];
  sprintf(batteryVoltageString, "%3.2fV", batteryVoltage);

  Serial.println("Display syncing badge.");
  u8g2Fonts.setFont(u8g2_fhpixelfont16px_12_gb2312);
  display.setPartialWindow(143, 0, 109, 18);
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

int connectWifi() {
  Serial.println("Connecting to WiFi...");
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(wifiSSID, wifiPass);
    int wifiRetry = 0;
    while (WiFi.status() != WL_CONNECTED && wifiRetry < 10) {
      wifiRetry++;
      delay(1000);
      Serial.println("Connecting to WiFi...");
    }

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(hotspotSSID, hotspotPass);
      int wifiRetry = 0;
      while (WiFi.status() != WL_CONNECTED && wifiRetry < 20) {
        wifiRetry++;
        delay(1000);
        Serial.println("Connecting to mobile hotspot...");
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to WiFi!");
      return 1;
    } else {
      Serial.println("Failed to connect to WiFi.");
      return 3;
    }
  } else {
    Serial.println("WiFi already connected.");
    return 2;
  }
}

void connectAndSyncTime() {
  if (connectWifi() > 2) return;

  Serial.println("Syncing time...");

  struct tm timeinfo;
  timeClient.begin();
  timeClient.forceUpdate();
  time_t epochTime = timeClient.getEpochTime();
  gmtime_r(&epochTime, &timeinfo);

  int16_t syncCount = 0;
  while (String(timeinfo.tm_year) == "70" && syncCount < 10) {
    syncCount++;
    Serial.println("Retry sync time...");
    timeClient.forceUpdate();
    time_t epochTime = timeClient.getEpochTime();
    gmtime_r(&epochTime, &timeinfo);
  }

  if (syncCount == 10) {
    Serial.println("Failed to sync time.");
  } else {
    time_t now = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = now };
    settimeofday(&tv, NULL);
    Serial.println("Time synced!");
  }
}

void connectAndUpdateClass() {
  serverConnected = false;

  if (connectWifi() > 2) return;

  Serial.println("Updating class...");
  // 连接到服务器
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient httpClient;

  if (httpClient.begin(classTimeAPI)) {
    u8_t httpCode = httpClient.GET();
    if (httpCode == HTTP_CODE_OK) {
      String response = httpClient.getString();
      Serial.println(response);
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, response);

      if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
      }

      std::string start_time, end_time, course, location;
      start_time = doc["start_time"].as<std::string>();
      strncpy(classStartTime, start_time.substr(0, 19).c_str(), 19);
      end_time = doc["end_time"].as<std::string>();
      strncpy(classEndTime, end_time.substr(0, 19).c_str(), 19);
      course = doc["course"].as<std::string>();
      if (substring(course, 13) == course) {
        strncpy(classCourse, course.c_str(), 39);
      } else {
        strncpy(classCourse, (substring(course, 13) + "…").c_str(), 39);
      }
      location = doc["location"].as<std::string>();
      strncpy(classLocation, substring(location, 7).c_str(), 12);

      if (start_time == "2023-01-01 00:00:00") {
        strncpy(classPeriod, ("下一节: " + end_time.substr(11, 5)).c_str(), 16);
      } else {
        strncpy(classPeriod, (start_time.substr(11, 5) + " - " + end_time.substr(11, 5)).c_str(), 16);
      }

      serverConnected = true;
    } else {
      Serial.printf("Connect to class_time api server failed, the http status code is:%u\n", httpCode);
    }
  } else {
    Serial.println("Failed to connect to class_time api server");
  }
  httpClient.end();
  client.stop();
}

void connectAndUpdateCurrentWeather() {
  serverConnected = false;

  if (connectWifi() > 2) return;

  Serial.println("Updating current weather...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient httpClient;

  String requestUrl = String(qWeatherURL) + "?location=" + String(qWeatherLocation) + "&key=" + String(qWeatherKey);
  if (httpClient.begin(requestUrl)) {
    u8_t httpCode = httpClient.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = httpClient.getString();
      Serial.println(payload);
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
      }

      if (doc["code"].as<String>() == "200") {
        JsonObject now = doc["now"];
        qWeatherTemp = atoi(now["temp"].as<String>().c_str());            // "4"
        strcpy(qWeatherText, now["text"].as<String>().c_str());           // "晴"
        strcpy(qWeatherWindDir, now["windDir"].as<String>().c_str());     // "西南风"
        qWeatherWindScale = atoi(now["windScale"].as<String>().c_str());  // "3"
        qWeatherHumidity = atoi(now["humidity"].as<String>().c_str());    // "16"

        // char weatherText[19] = "";
        // if (substring(std::string(qWeatherText), 6) == std::string(qWeatherText)) {
        //   strcpy(weatherText, qWeatherText);
        // } else {
        //   strcpy(weatherText, (substring(std::string(qWeatherText), 5) + "…").c_str());
        // }

        // Serial.print("weatherText: ");
        // Serial.println(weatherText);

        char windScale[6] = "";
        if (qWeatherWindScale != 0) {
          sprintf(windScale, "%d级", qWeatherWindScale);
        }

        sprintf(qWeatherString1, "%.6s%d℃", qWeatherText, qWeatherTemp);
        sprintf(qWeatherString2, "%s%s", qWeatherWindDir, windScale);
        sprintf(qWeatherString3, "湿度%d%%", qWeatherHumidity);

        serverConnected = true;
      }
    } else {
      Serial.printf("Connect to weather api server failed, the http status code is:%u\n", httpCode);
    }
  } else {
    Serial.println("Get current weather failed");
  }
  httpClient.end();
  client.stop();
}

void connectAndUpdateClassTimeTable() {
  if (connectWifi() > 2) return;

  // 初始化SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("无法挂载SPIFFS");
    return;
  }

  // 创建SPIFFS文件
  File file = SPIFFS.open("/classtimetable.csv", FILE_WRITE);
  if (!file) {
    Serial.println("无法创建文件");
    return;
  }

  // 创建HTTP客户端对象
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient httpClient;

  // 发送GET请求以下载CSV文件
  httpClient.begin("https://https://api.example.com/class_time_csv.php");

  // 开始下载并将数据写入文件
  u8_t httpCode = httpClient.GET();
  if (httpCode == HTTP_CODE_OK) {
    // 下载成功，覆盖旧文件
    file.seek(0, SeekSet);
    httpClient.writeToStream(&file);
    Serial.println("文件下载成功");
  } else {
    Serial.printf("文件下载失败，HTTP错误代码：%d\n", httpCode);
  }

  // 关闭HTTP客户端和文件
  httpClient.end();
  client.stop();
  file.close();
}

// 将时间字符串转换为时间戳
time_t getTimeStamp(char* timeString) {
  // 定义时间格式
  char format[] = "%Y-%m-%d %H:%M:%S";
  struct tm timeinfo = { 0 };
  strptime(timeString, format, &timeinfo);
  time_t timeStamp = mktime(&timeinfo);
  return timeStamp;
}

std::string substring(std::string str, int length) {
  std::string subString = "";
  int count = 0;  // 记录已输出的字符数
  for (auto it = str.begin(); it != str.end() && count < length; ++it) {
    if ((*it & 0xC0) != 0x80) {        // 如果当前字符是一个多字节字符的第一个字节
      if (count == length - 1) break;  // 如果该字符是第length个字符，则直接退出循环
      count++;
    }
    subString += *it;
  }
  return subString;
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1: Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP: Serial.println("Wakeup caused by ULP program"); break;
    default: Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason); break;
  }
}

void deepSleep2NextWholeMinute() {
  time_t sleepNow;
  struct tm sleepTimeInfo;
  time(&sleepNow);
  localtime_r(&sleepNow, &sleepTimeInfo);

  // Calculate the number of milliseconds until the next whole minute
  uint64_t sleepTime = (60 - sleepTimeInfo.tm_sec) * 1000;
  sleepTime += 1000 - (esp_timer_get_time() % 1000);
  if (sleepTime < 0) sleepTime = 0;

  Serial.printf("Sleeping for %d ms\r\n", sleepTime);

  esp_sleep_enable_timer_wakeup(sleepTime * 1000ULL);
  esp_deep_sleep_start();
}