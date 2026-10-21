#include "storage.h"
#include <Preferences.h>
#include <time.h>
#include <M5CoreInk.h>

static Preferences s_prefs;
static const char *PREFS_NAMESPACE = "env_data";
static const char *KEY_RECORDS = "records";
static const char *KEY_COUNT = "count";

// 1. NVS 持久化 7 天 (168 小时) 整点历史记录
static EnvRecord s_history[MAX_HISTORY_RECORDS];
static uint16_t s_count = 0;
static uint16_t s_head = 0; // 下一个写入位置

// 2. RTC 内存高精实时采样环形缓冲区 (Deep Sleep 期间不断电，保持连续历史曲线)
RTC_DATA_ATTR static EnvRecord s_recent[MAX_RECENT_RECORDS];
RTC_DATA_ATTR static uint16_t s_recentCount = 0;
RTC_DATA_ATTR static uint16_t s_recentHead = 0;
RTC_DATA_ATTR static uint32_t s_rtcMagic = 0;
static const uint32_t RTC_STORAGE_MAGIC = 0x43494E4B; // 'CINK'

uint32_t getSystemTimestamp() {
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    M5.rtc.GetTime(&t);
    M5.rtc.GetDate(&d);

    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(timeinfo));
    timeinfo.tm_year = (d.Year >= 2000) ? (d.Year - 1900) : (d.Year + 100);
    timeinfo.tm_mon  = (d.Month >= 1 && d.Month <= 12) ? (d.Month - 1) : 0;
    timeinfo.tm_mday = (d.Date >= 1 && d.Date <= 31) ? d.Date : 1;
    timeinfo.tm_hour = t.Hours;
    timeinfo.tm_min  = t.Minutes;
    timeinfo.tm_sec  = t.Seconds;

    time_t epoch = mktime(&timeinfo);
    if (epoch > 1577836800) { // 2020年以后的合法时间戳
        return (uint32_t)epoch;
    }
    return (uint32_t)(millis() / 1000UL);
}

void initStorage() {
    Serial.println("[Storage] Initializing NVS storage for environmental records...");
    if (!s_prefs.begin(PREFS_NAMESPACE, false)) {
        Serial.println("[Storage] Failed to open Preferences namespace 'env_data'!");
        return;
    }

    // 校验 RTC 实时环形缓冲区魔数，初次上电时重置
    if (s_rtcMagic != RTC_STORAGE_MAGIC || s_recentCount > MAX_RECENT_RECORDS || s_recentHead >= MAX_RECENT_RECORDS) {
        Serial.println("[Storage] RTC memory cold reset: Initializing recent buffer.");
        s_recentCount = 0;
        s_recentHead = 0;
        s_rtcMagic = RTC_STORAGE_MAGIC;
    } else {
        Serial.printf("[Storage] Restored %u recent records across Deep Sleep from RTC memory.\n", s_recentCount);
    }

    // 兼容读取 UShort 与老版本 UChar
    s_count = s_prefs.getUShort(KEY_COUNT, s_prefs.getUChar(KEY_COUNT, 0));
    if (s_count > MAX_HISTORY_RECORDS) {
        s_count = MAX_HISTORY_RECORDS;
    }

    if (s_count > 0) {
        size_t expectedBytes = s_count * sizeof(EnvRecord);
        size_t readBytes = s_prefs.getBytes(KEY_RECORDS, s_history, expectedBytes);
        if (readBytes == expectedBytes) {
            uint32_t nowSec = getSystemTimestamp();
            // 清洗历史脏数据并补充时间戳
            uint16_t validCount = 0;
            for (uint16_t i = 0; i < s_count; ++i) {
                if (s_history[i].humi >= 99.9f || s_history[i].humi <= 0.0f ||
                    s_history[i].temp < -20.0f || s_history[i].temp > 60.0f) {
                    continue; // 忽略历史脏点
                }
                // 若历史时间戳异常，回溯修复为粗略时间戳
                if (s_history[i].timestamp < 1000000000UL && nowSec > 1577836800UL) {
                    s_history[i].timestamp = nowSec - (s_count - i) * 3600UL;
                }
                s_history[validCount++] = s_history[i];
            }
            s_count = validCount;
            s_head = s_count % MAX_HISTORY_RECORDS;
            Serial.printf("[Storage] Successfully restored %u valid hourly records from NVS.\n", s_count);
        } else {
            Serial.println("[Storage] Stored record size mismatch, resetting buffer.");
            s_count = 0;
            s_head = 0;
        }
    } else {
        Serial.println("[Storage] No existing records found in NVS. Starting fresh.");
        s_count = 0;
        s_head = 0;
    }
}

void flushStorageToNVS() {
    if (s_count == 0) return;

    static EnvRecord sorted[MAX_HISTORY_RECORDS];
    uint16_t count = getHistoryRecords(sorted, MAX_HISTORY_RECORDS);

    size_t writeBytes = count * sizeof(EnvRecord);
    size_t written = s_prefs.putBytes(KEY_RECORDS, sorted, writeBytes);
    s_prefs.putUShort(KEY_COUNT, count);

    Serial.printf("[Storage] Persisted %u records (%u bytes) to NVS.\n",
                  count, (unsigned int)written);
}

void appendHourlyRecord(const EnvRecord &rec) {
    EnvRecord r = rec;
    if (r.timestamp < 1000000000UL) {
        r.timestamp = getSystemTimestamp();
    }

    if (s_count < MAX_HISTORY_RECORDS) {
        s_history[s_head] = r;
        s_head = (s_head + 1) % MAX_HISTORY_RECORDS;
        s_count++;
    } else {
        s_history[s_head] = r;
        s_head = (s_head + 1) % MAX_HISTORY_RECORDS;
    }

    Serial.printf("[Storage] Appended hourly record #%u (ts=%u): T:%.1fC, H:%.1f%%\n",
                  s_count, r.timestamp, r.temp, r.humi);
    flushStorageToNVS();
}

void appendRecentRecord(const EnvRecord &rec) {
    if (rec.humi <= 0.0f || rec.humi >= 99.9f || rec.temp < -20.0f || rec.temp > 60.0f) {
        return;
    }

    EnvRecord r = rec;
    if (r.timestamp < 1000000000UL) {
        r.timestamp = getSystemTimestamp();
    }

    s_recent[s_recentHead] = r;
    s_recentHead = (s_recentHead + 1) % MAX_RECENT_RECORDS;
    if (s_recentCount < MAX_RECENT_RECORDS) {
        s_recentCount++;
    }
}

uint16_t getHistoryRecords(EnvRecord *outArray, uint16_t maxLen) {
    if (!outArray || maxLen == 0 || s_count == 0) {
        return 0;
    }

    uint16_t n = (s_count < maxLen) ? s_count : maxLen;
    if (s_count < MAX_HISTORY_RECORDS) {
        for (uint16_t i = 0; i < n; ++i) {
            outArray[i] = s_history[i];
        }
    } else {
        uint16_t startIdx = (s_head + MAX_HISTORY_RECORDS - n) % MAX_HISTORY_RECORDS;
        for (uint16_t i = 0; i < n; ++i) {
            outArray[i] = s_history[(startIdx + i) % MAX_HISTORY_RECORDS];
        }
    }

    return n;
}

bool getLatestHistoryRecord(EnvRecord &outRec) {
    if (s_count == 0) return false;
    uint16_t lastIdx = (s_head + MAX_HISTORY_RECORDS - 1) % MAX_HISTORY_RECORDS;
    outRec = s_history[lastIdx];
    return true;
}

uint16_t getRecordsInWindow(EnvRecord *outArray, uint16_t maxLen, uint32_t windowStart, uint32_t windowEnd) {
    if (!outArray || maxLen == 0) return 0;

    uint16_t collected = 0;

    // 1. 从 NVS 历史整点中按时序筛选
    static EnvRecord hourly[MAX_HISTORY_RECORDS];
    uint16_t hCount = getHistoryRecords(hourly, MAX_HISTORY_RECORDS);
    for (uint16_t i = 0; i < hCount && collected < maxLen; ++i) {
        if (hourly[i].timestamp >= windowStart && hourly[i].timestamp <= windowEnd) {
            outArray[collected++] = hourly[i];
        }
    }

    // 2. 从 RTC 保持的高频采样缓冲中筛选
    if (s_recentCount > 0) {
        uint16_t startIdx = (s_recentHead + MAX_RECENT_RECORDS - s_recentCount) % MAX_RECENT_RECORDS;
        for (uint16_t i = 0; i < s_recentCount && collected < maxLen; ++i) {
            const EnvRecord &r = s_recent[(startIdx + i) % MAX_RECENT_RECORDS];
            if (r.timestamp >= windowStart && r.timestamp <= windowEnd) {
                // 如果与最近已添加的点时间过近(<= 3秒)，跳过冗余点
                if (collected > 0 && r.timestamp <= outArray[collected - 1].timestamp + 3) {
                    continue;
                }
                outArray[collected++] = r;
            }
        }
    }

    // 3. 排序 (快速单调递增保证)
    for (uint16_t i = 0; i < collected; ++i) {
        for (uint16_t j = i + 1; j < collected; ++j) {
            if (outArray[j].timestamp < outArray[i].timestamp) {
                EnvRecord tmp = outArray[i];
                outArray[i] = outArray[j];
                outArray[j] = tmp;
            }
        }
    }

    return collected;
}

uint16_t getHistoryCount() {
    return s_count;
}
