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
    bool hasLeftPrev = false;
    EnvRecord leftPrev;
    bool hasRightNext = false;
    EnvRecord rightNext;

    // 1. 从 NVS 历史整点中按时序筛选，同时捕获视窗外最近的前驱/后继边界点
    static EnvRecord hourly[MAX_HISTORY_RECORDS];
    uint16_t hCount = getHistoryRecords(hourly, MAX_HISTORY_RECORDS);
    for (uint16_t i = 0; i < hCount; ++i) {
        if (hourly[i].timestamp >= windowStart && hourly[i].timestamp <= windowEnd) {
            if (collected < maxLen - 2) {
                outArray[collected++] = hourly[i];
            }
        } else if (hourly[i].timestamp < windowStart) {
            if (!hasLeftPrev || hourly[i].timestamp > leftPrev.timestamp) {
                leftPrev = hourly[i];
                hasLeftPrev = true;
            }
        } else if (hourly[i].timestamp > windowEnd) {
            if (!hasRightNext || hourly[i].timestamp < rightNext.timestamp) {
                rightNext = hourly[i];
                hasRightNext = true;
            }
        }
    }

    // 2. 从 RTC 保持的高频采样缓冲中筛选，同时补充捕获更贴近的边界点
    if (s_recentCount > 0) {
        uint16_t startIdx = (s_recentHead + MAX_RECENT_RECORDS - s_recentCount) % MAX_RECENT_RECORDS;
        for (uint16_t i = 0; i < s_recentCount; ++i) {
            const EnvRecord &r = s_recent[(startIdx + i) % MAX_RECENT_RECORDS];
            if (r.timestamp >= windowStart && r.timestamp <= windowEnd) {
                if (collected < maxLen - 2) {
                    outArray[collected++] = r;
                }
            } else if (r.timestamp < windowStart) {
                if (!hasLeftPrev || r.timestamp > leftPrev.timestamp) {
                    leftPrev = r;
                    hasLeftPrev = true;
                }
            } else if (r.timestamp > windowEnd) {
                if (!hasRightNext || r.timestamp < rightNext.timestamp) {
                    rightNext = r;
                    hasRightNext = true;
                }
            }
        }
    }

    // 若视窗内完全无采样点，但视窗外有前驱点，以该状态生成贯穿全屏的水平参考线
    if (collected == 0) {
        if (hasLeftPrev) {
            EnvRecord aLeft = leftPrev;
            aLeft.timestamp = windowStart;
            EnvRecord aRight = leftPrev;
            aRight.timestamp = windowEnd;
            outArray[0] = aLeft;
            outArray[1] = aRight;
            return 2;
        }
        return 0;
    }

    // 3. 严格按时间戳单调升序排序
    for (uint16_t i = 0; i < collected - 1; ++i) {
        for (uint16_t j = i + 1; j < collected; ++j) {
            if (outArray[j].timestamp < outArray[i].timestamp) {
                EnvRecord tmp = outArray[i];
                outArray[i] = outArray[j];
                outArray[j] = tmp;
            }
        }
    }

    // 4. 排序后去除紧邻的重复或冗余采样点 (时间差 <= 3秒的相邻点只保留 1 个)
    uint16_t uniqueCount = 1;
    for (uint16_t i = 1; i < collected; ++i) {
        if (outArray[i].timestamp > outArray[uniqueCount - 1].timestamp + 3) {
            outArray[uniqueCount++] = outArray[i];
        }
    }

    // 5. 左边界平滑锚定 (核心修复：彻底消除 1h / 10m 等微观视窗左侧空白断层)
    if (outArray[0].timestamp > windowStart && uniqueCount < maxLen) {
        EnvRecord leftAnchor = outArray[0];
        leftAnchor.timestamp = windowStart;
        if (hasLeftPrev && (outArray[0].timestamp > leftPrev.timestamp)) {
            float span = (float)(outArray[0].timestamp - leftPrev.timestamp);
            float ratio = (float)(windowStart - leftPrev.timestamp) / span;
            leftAnchor.temp = leftPrev.temp + ratio * (outArray[0].temp - leftPrev.temp);
            leftAnchor.humi = leftPrev.humi + ratio * (outArray[0].humi - leftPrev.humi);
            leftAnchor.press = leftPrev.press + ratio * (outArray[0].press - leftPrev.press);
        }
        for (int i = uniqueCount; i > 0; --i) {
            outArray[i] = outArray[i - 1];
        }
        outArray[0] = leftAnchor;
        uniqueCount++;
    }

    // 6. 右边界闭环锚定 (将折线稳稳闭环连至右侧坐标轴)
    if (outArray[uniqueCount - 1].timestamp < windowEnd && uniqueCount < maxLen) {
        EnvRecord rightAnchor = outArray[uniqueCount - 1];
        rightAnchor.timestamp = windowEnd;
        if (hasRightNext && (rightNext.timestamp > outArray[uniqueCount - 1].timestamp)) {
            float span = (float)(rightNext.timestamp - outArray[uniqueCount - 1].timestamp);
            float ratio = (float)(windowEnd - outArray[uniqueCount - 1].timestamp) / span;
            rightAnchor.temp = outArray[uniqueCount - 1].temp + ratio * (rightNext.temp - outArray[uniqueCount - 1].temp);
            rightAnchor.humi = outArray[uniqueCount - 1].humi + ratio * (rightNext.humi - outArray[uniqueCount - 1].humi);
            rightAnchor.press = outArray[uniqueCount - 1].press + ratio * (rightNext.press - outArray[uniqueCount - 1].press);
        }
        outArray[uniqueCount++] = rightAnchor;
    }

    return uniqueCount;
}

uint16_t getHistoryCount() {
    return s_count;
}
