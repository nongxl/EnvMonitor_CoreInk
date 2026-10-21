#include "time_sync.h"
#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

#include <WiFi.h>
#include <WiFiMulti.h>
#include <esp_wifi.h>
#include <time.h>
#include <M5CoreInk.h>

static void powerOffWiFi() {
    Serial.println("[Wi-Fi] Disconnecting and powering down RF...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    Serial.println("[Wi-Fi] RF transceiver completely shut down.");
}

bool syncNtpTime(uint32_t timeoutMs) {
    if (WIFI_AP_COUNT == 0) {
        Serial.println("[NTP] No AP configured in WIFI_AP_LIST, skip NTP sync.");
        powerOffWiFi();
        return false;
    }

    Serial.println("========================================");
    Serial.printf("[Wi-Fi] Starting WiFiMulti with %u AP candidates...\n", (unsigned int)WIFI_AP_COUNT);

    WiFiMulti wifiMulti;
    for (size_t i = 0; i < WIFI_AP_COUNT; ++i) {
        if (WIFI_AP_LIST[i].ssid && strlen(WIFI_AP_LIST[i].ssid) > 0) {
            wifiMulti.addAP(WIFI_AP_LIST[i].ssid, WIFI_AP_LIST[i].password);
            Serial.printf("  - Added AP: %s\n", WIFI_AP_LIST[i].ssid);
        }
    }

    WiFi.mode(WIFI_STA);
    uint32_t startMs = millis();
    bool connected = false;

    Serial.printf("[Wi-Fi] Connecting (timeout: %u ms)...\n", timeoutMs);
    while (millis() - startMs < timeoutMs) {
        if (wifiMulti.run() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(150);
        Serial.print(".");
    }
    Serial.println();

    if (!connected) {
        Serial.println("[Wi-Fi] Connection timed out! Continuing with RTC offline time.");
        powerOffWiFi();
        return false;
    }

    Serial.printf("[Wi-Fi] Connected to '%s', IP: %s, RSSI: %d dBm\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());

    // 配置 NTP 服务器 (UTC+8，不设夏令时)
    const long gmtOffset_sec = 8 * 3600;
    const int daylightOffset_sec = 0;
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "ntp1.aliyun.com", "time.nist.gov");

    Serial.println("[NTP] Fetching network time via NTP...");
    struct tm timeinfo;
    bool ntpSuccess = getLocalTime(&timeinfo, 5000); // 最多等 5 秒

    if (ntpSuccess) {
        Serial.printf("[NTP] Got time: %04d-%02d-%02d %02d:%02d:%02d (weekday: %d)\n",
                      timeinfo.tm_year + 1900,
                      timeinfo.tm_mon + 1,
                      timeinfo.tm_mday,
                      timeinfo.tm_hour,
                      timeinfo.tm_min,
                      timeinfo.tm_sec,
                      timeinfo.tm_wday);

        // 写入硬件 RTC (BM8563)
        RTC_TimeTypeDef rtcTime(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        RTC_DateTypeDef rtcDate(timeinfo.tm_wday,
                                timeinfo.tm_mon + 1,
                                timeinfo.tm_mday,
                                timeinfo.tm_year + 1900);

        M5.rtc.SetTime(&rtcTime);
        M5.rtc.SetDate(&rtcDate);

        // 验证读回
        RTC_TimeTypeDef checkTime;
        RTC_DateTypeDef checkDate;
        M5.rtc.GetTime(&checkTime);
        M5.rtc.GetDate(&checkDate);
        Serial.printf("[BM8563] Hardware RTC updated: %04d-%02d-%02d %02d:%02d:%02d\n",
                      checkDate.Year, checkDate.Month, checkDate.Date,
                      checkTime.Hours, checkTime.Minutes, checkTime.Seconds);
    } else {
        Serial.println("[NTP] Failed to obtain time from NTP servers.");
    }

    // 彻底关断射频
    powerOffWiFi();
    Serial.println("========================================");

    return ntpSuccess;
}
