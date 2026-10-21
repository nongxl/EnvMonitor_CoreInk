#pragma once
#include <stddef.h>

struct WifiCredential {
    const char* ssid;
    const char* password;
};

// 候选 Wi-Fi 列表（可自由增减，优先连接信号最强或最先搜到的已知热点）
// 复制本文件为 config.h 并填入您的真实 Wi-Fi 热点信息
const WifiCredential WIFI_AP_LIST[] = {
    {"Your_WiFi_SSID_1", "Your_WiFi_Password_1"},
    {"Your_WiFi_SSID_2", "Your_WiFi_Password_2"}
};

const size_t WIFI_AP_COUNT = sizeof(WIFI_AP_LIST) / sizeof(WIFI_AP_LIST[0]);
