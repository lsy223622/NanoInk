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

// 自定义设置
const char wifiSSID[] = "REDACTED";
const char wifiPass[] = "REDACTED";
const char hotspotSSID[] = "REDACTED";
const char hotspotPass[] = "REDACTED";
const char qWeatherURL[] = "https://api.example.com/proxyforqweather.php";
const char qWeatherKey[] = "REDACTED";
const char qWeatherLocation[] = "000000000";
const char classTimeAPI[] = "https://api.example.com/class_time.php";
const int WIFI_RETRY_LIMIT = 5;
const int HOTSPOT_RETRY_LIMIT = 10;
const int WIFI_TIMEOUT = 1000;

// enter your hmacKey (10 digits)
uint8_t hmacKey[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
TOTP totp = TOTP(hmacKey, 10);

WiFiClientSecure* getSecureClient() {
  static WiFiClientSecure client;  // 静态对象复用
  client.setInsecure();
  client.setTimeout(10);  // 设置10秒超时
  return &client;
}

void setup() {
  Serial.begin(115200);   // 初始化串口
  print_wakeup_reason();  // 打印睡眠唤醒原因

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

  // if (rtc.lostPower()) {
  //   Serial.println("RTC lost power, let's set the time!");
  //   rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  // }

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

    updateClass();  // 更新课程
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

  now = rtc.now();  // 更新时间

  // 如果课程结束时间小于当前时间，则更新课程
  while (getTimeStamp(classEndTime) < now.unixtime()) {
    delFirstLine("/classtimetable.csv");  // 删除第一行
    updateClass();                        // 更新课程
  }

  mainDisplay(now.minute() % 30 != 0);  // 更新屏幕

  // 每小时联网同步数据
  if (now.minute() == 0 && connectWifi() < 3) {
    displaySyncingBadge();                                                                         // 每小时联网同步时间
    now = rtc.now();                                                                               // 更新时间
    if (now.hour() == 12 && now.minute() == 0) syncTime();                                         // 每天12点联网同步时间
    if (now.dayOfTheWeek() == 0 && now.hour() == 12 && now.minute() == 0) updateClassTimeTable();  // 每周日12点联网同步课程表
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

  now = rtc.now();                                                                                              // 更新时间
  char currentDateString[27];                                                                                   // 日期格式：2021-01-01 星期一
  sprintf(currentDateString, "%4d-%d-%d %s", now.year(), now.month(), now.day(), WEEKDAY[now.dayOfTheWeek()]);  // 生成日期字符串
  char currentTimeString[8];                                                                                    // 时间格式：00:00
  sprintf(currentTimeString, "%02d:%02d", now.hour(), now.minute());                                            // 生成时间字符串

  char totpCode[7] = "";                                      // TOTP动态验证码
  Serial.println(now.unixtime() - 8 * 3600);                  // 东八区时间戳
  strcpy(totpCode, totp.getCode(now.unixtime() - 8 * 3600));  // 生成动态验证码
  Serial.println(totpCode);                                   // 打印动态验证码

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
  Serial.println("Syncing time...");  // 打印日志

  timeClient.begin();        // 启动NTP客户端
  timeClient.forceUpdate();  // 强制更新时间

  // 尝试同步时间，最多尝试10次
  int16_t syncCount = 0;
  while (syncCount < 10 && timeClient.getEpochTime() < 100) {
    Serial.println("Trying to sync time...");  // 打印日志
    timeClient.forceUpdate();                  // 强制更新时间
    syncCount++;                               // 尝试次数加一
  }

  // 如果同步失败，则打印日志并返回
  if (syncCount == 10) {
    Serial.println("Failed to sync time.");
    return;
  }

  rtc.adjust(DateTime(timeClient.getEpochTime()));  // 更新RTC时间
  Serial.println("DS3231 time updated from NTP");   // 打印日志
}

// 更新课程
void updateClass() {
  File file = SPIFFS.open("/classtimetable.csv", FILE_READ);  // 打开文件

  // 如果文件不存在，则打印日志并返回
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }

  String firstLine = file.readStringUntil('\n');  // 读取第一行

  file.close();  // 关闭文件

  // 解析CSV格式的数据
  String class_no, start_time, end_time, course, location;  // 定义变量
  int comma1 = firstLine.indexOf(',');                      // 查找第一个逗号
  int comma2 = firstLine.indexOf(',', comma1 + 1);          // 查找第二个逗号
  int comma3 = firstLine.indexOf(',', comma2 + 1);          // 查找第三个逗号
  int comma4 = firstLine.indexOf(',', comma3 + 1);          // 查找第四个逗号

  class_no = firstLine.substring(0, comma1);             // 从第一个字符开始截取到第一个逗号
  start_time = firstLine.substring(comma1 + 1, comma2);  // 从第一个逗号后一个字符开始截取到第二个逗号
  end_time = firstLine.substring(comma2 + 1, comma3);    // 从第二个逗号后一个字符开始截取到第三个逗号
  course = firstLine.substring(comma3 + 1, comma4);      // 从第三个逗号后一个字符开始截取到第四个逗号
  location = firstLine.substring(comma4 + 1);            // 从第四个逗号后一个字符开始截取到最后一个字符

  start_time.replace("\"", "");  // 去除引号
  end_time.replace("\"", "");    // 去除引号
  location.replace("\"", "");    // 去除引号

  strncpy(classStartTime, start_time.substring(0, 19).c_str(), 19);  // 截取字符串
  strncpy(classEndTime, end_time.substring(0, 19).c_str(), 19);      // 截取字符串

  // 裁剪过长的课程名
  if (subStringX(course, 13) == course) {
    strncpy(classCourse, course.c_str(), 39);
  } else {
    strncpy(classCourse, (subStringX(course, 13) + "…").c_str(), 39);
  }

  strncpy(classLocation, subStringX(location, 7).c_str(), 12);  // 裁剪过长的地点名

  now = rtc.now();  // 更新时间

  // 如果课程开始时间大于当前时间，说明现在是下课，将课程开始时间设置为 2023-01-01 00:00:00
  if (getTimeStamp(classStartTime) > now.unixtime()) {
    strncpy(classEndTime, classStartTime, 19);
    strncpy(classStartTime, "2023-01-01 00:00:00", 20);
  }

  // 判断当前是否上课
  if (String(classStartTime) == "2023-01-01 00:00:00") {
    strncpy(classPeriod, ("下一节: " + String(classEndTime).substring(11, 16)).c_str(), 16);  // 如果当前不上课，则显示下一节课的开始时间
  } else {
    strncpy(classPeriod, (String(classStartTime).substring(11, 16) + " - " + String(classEndTime).substring(11, 16)).c_str(), 16);  // 如果当前上课，则显示当前课程的开始时间和结束时间
  }
}

// 更新天气
void updateCurrentWeather() {
  serverConnected = false;  // 将服务器连接状态设置为false

  Serial.println("Updating current weather...");  // 打印日志

  WiFiClientSecure* client = getSecureClient();
  HTTPClient http;

  String requestUrl = String(qWeatherURL) + "?location=" + String(qWeatherLocation) + "&key=" + String(qWeatherKey);  // 拼接请求URL

  if (!http.begin(*client, requestUrl)) {
    Serial.println("HTTP setup failed");
    return;
  }

  http.setTimeout(10000);  // 10秒超时

  // 发送GET请求以获取天气数据
  try {
    int httpCode = http.GET();  // 发送GET请求

    // 如果请求成功，则解析JSON数据
    if (httpCode == HTTP_CODE_OK) {
      String payload = httpClient.getString();                     // 获取响应内容
      const size_t JSON_CAPACITY = 2048;                           // 更大的缓冲区
      StaticJsonDocument<JSON_CAPACITY> doc;                       // 使用静态分配，避免堆内存碎片
      DeserializationError error = deserializeJson(doc, payload);  // 解析JSON数据

      // 如果解析失败，则打印日志并返回
      if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
      }

      // 如果解析成功，则将数据写入变量
      if (doc["code"].as<String>() == "200") {
        JsonObject now = doc["now"];                                      // 获取当前天气数据
        qWeatherTemp = atoi(now["temp"].as<String>().c_str());            // "4"
        strcpy(qWeatherText, now["text"].as<String>().c_str());           // "晴"
        strcpy(qWeatherWindDir, now["windDir"].as<String>().c_str());     // "西南风"
        qWeatherWindScale = atoi(now["windScale"].as<String>().c_str());  // "3"
        qWeatherHumidity = atoi(now["humidity"].as<String>().c_str());    // "16"

        char windScale[10] = "";                                                    // 风力等级
        if (qWeatherWindScale != 0) sprintf(windScale, "%d级", qWeatherWindScale);  // 如果风力等级不为0，则拼接风力等级字符串

        sprintf(qWeatherString1, "%.6s%d℃", qWeatherText, qWeatherTemp);  // 拼接天气字符串
        sprintf(qWeatherString2, "%s%s", qWeatherWindDir, windScale);     // 拼接风向字符串
        sprintf(qWeatherString3, "湿度%d%%", qWeatherHumidity);           // 拼接湿度字符串

        serverConnected = true;  // 将服务器连接状态设置为true
      }
    } else {
      Serial.printf("Connect to weather api server failed, the http status code is:%u\n", httpCode);  // 打印日志
    }
  } catch (const std::exception& e) {
    Serial.printf("HTTP request failed: %s\n", e.what());
    Serial.println("Get current weather failed");
  }
  http.end();  // 关闭HTTP客户端
}

// 更新课程表
void updateClassTimeTable() {
  File file = SPIFFS.open("/classtimetable.csv", FILE_WRITE);  // 创建SPIFFS文件

  // 如果文件不存在，则打印日志并返回
  if (!file) {
    Serial.println("无法写入文件");
    return;
  }

  WiFiClientSecure client;  // 创建WiFi客户端对象
  client.setInsecure();     // 设置客户端为不安全模式
  HTTPClient httpClient;    // 创建HTTP客户端对象

  httpClient.begin("https://api.example.com/class_time_csv.php");  // 发送GET请求以下载CSV文件
  u8_t httpCode = httpClient.GET();                                  // 发送GET请求

  // 如果请求成功，则将数据写入文件
  if (httpCode == HTTP_CODE_OK) {
    file.seek(0, SeekSet);            // 将文件指针移动到文件开头
    httpClient.writeToStream(&file);  // 将响应内容写入文件
    Serial.println("文件下载成功");   // 打印日志
  } else {
    Serial.printf("文件下载失败，HTTP错误代码：%d\n", httpCode);  // 打印日志
  }

  httpClient.end();  // 关闭HTTP客户端
  client.stop();     // 关闭WiFi客户端
  file.close();      // 关闭文件
}

// 将时间字符串转换为时间戳
time_t getTimeStamp(char* timeString) {
  char format[] = "%Y-%m-%d %H:%M:%S";      // 定义时间格式
  struct tm timeinfo = { 0 };               // 定义时间结构体
  strptime(timeString, format, &timeinfo);  // 将时间字符串转换为时间结构体
  time_t timeStamp = mktime(&timeinfo);     // 将时间结构体转换为时间戳
  return timeStamp;                         // 返回时间戳
}

// 截取字符串前length个字符
String subStringX(String str, int length) {
  String subStringX = "";  // 存储截取后的字符串
  int count = 0;           // 记录已输出的字符数

  // 遍历字符串
  for (auto it = str.begin(); it != str.end() && count < length; ++it) {
    if ((*it & 0xC0) != 0x80) {        // 如果当前字符是一个多字节字符的第一个字节
      if (count == length - 1) break;  // 如果该字符是第length个字符，则直接退出循环
      count++;                         // 已输出字符数加一
    }
    subStringX += *it;  // 将当前字符添加到截取后的字符串中
  }
  return subStringX;  // 返回截取后的字符串
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
  now = rtc.now();

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

// 写入文件
void writeFile(const char* path, const char* content) {
  Serial.printf("Writing to file: %s\n", path);  // 打印日志

  File file = SPIFFS.open(path, FILE_WRITE);  // 打开文件

  // 如果文件不存在，则打印日志并返回
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }

  // 如果写入成功，则打印日志
  if (file.print(content)) {
    Serial.println("File written successfully");
  } else {
    Serial.println("Write failed");
  }

  file.close();  // 关闭文件
}

// 读取文件
void readFile(const char* path) {
  Serial.printf("Reading file: %s\n", path);  // 打印日志

  File file = SPIFFS.open(path, FILE_READ);  // 打开文件

  // 如果文件不存在，则打印日志并返回
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }

  // 如果文件存在，则打印文件内容
  Serial.println("File content:");
  while (file.available()) {
    Serial.print((char)file.read());
  }
  Serial.println();

  file.close();  // 关闭文件
}

// 下载文件
void downloadFile(const char* url, const char* path) {
  HTTPClient http;  // 创建HTTP客户端对象

  Serial.print("Downloading file from URL: ");  // 打印日志
  Serial.println(url);                          // 打印日志

  // 发送GET请求以下载文件
  if (http.begin(url)) {
    int httpCode = http.GET();

    // 如果请求成功，则将响应内容写入文件
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK) {
        File file = SPIFFS.open(path, FILE_WRITE);
        if (file) {
          http.writeToStream(&file);
          file.close();
          Serial.println("File downloaded and saved to SPIFFS");
        } else {
          Serial.println("Failed to open file for writing");
        }
      } else {
        Serial.print("HTTP request failed with error code: ");
        Serial.println(httpCode);
      }
    } else {
      Serial.println("Connection failed");
    }

    http.end();
  } else {
    Serial.println("HTTP client setup failed");
  }
}

// 更新电池电压
void updateBatteryVoltage() {
  int voltageADC = analogRead(36);                               // 读取ADC值
  float batteryVoltageRead = voltageADC * 1.1 / 455;             // 将读取到的ADC值转换为电压值
  Serial.print("Battery voltage: ");                             // 打印电压值
  char batteryVoltagePrecise[8];                                 // 定义电压值字符串
  sprintf(batteryVoltagePrecise, "%5.4fV", batteryVoltageRead);  // 将电压值转换为字符串
  Serial.println(String(batteryVoltagePrecise));                 // 打印电压值
  sprintf(batteryVoltageString, "%3.2fV", batteryVoltageRead);   // 将电压值转换为字符串
}

// 解析日期时间字符串
DateTime parseDateTime(const char* dateTimeStr) {
  int year, month, day, hour, minute, second;                                              // 定义变量
  sscanf(dateTimeStr, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);  // 解析日期时间字符串
  return DateTime(year, month, day, hour, minute, second);                                 // 返回DateTime对象
}

// 删除文件第一行
void delFirstLine(char* path) {
  // 读取文件
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }

  String content = "";                       // 定义变量
  String line = file.readStringUntil('\n');  // 读取第一行

  // 读取剩余行
  while (file.available()) {
    content += file.readStringUntil('\n');
  }

  file.close();  // 关闭文件

  content = content.substring(content.indexOf('\n') + 1);  // 删除第一行

  // 将剩余行写入文件
  file = SPIFFS.open(path, FILE_WRITE);
  if (file) {
    file.print(content);                          // 写入文件
    file.close();                                 // 关闭文件
    Serial.println("File updated successfully");  // 打印日志
  } else {
    Serial.println("Failed to open file for writing");  // 打印日志
  }
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
