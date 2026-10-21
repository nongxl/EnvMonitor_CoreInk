#pragma once
#include <Arduino.h>

/**
 * @brief 使用 Wi-Fi Multi 轮询候选热点，进行 NTP 对时并写入 BM8563 RTC，最后彻底关闭射频
 * 
 * @param timeoutMs Wi-Fi 连接最大等待时间（毫秒），默认 8000ms
 * @return true 对时并写入 RTC 成功
 * @return false 对时失败（超时或网络不可用，继续使用 RTC 现有时间）
 */
bool syncNtpTime(uint32_t timeoutMs = 8000);
