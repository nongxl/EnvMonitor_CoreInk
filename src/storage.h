#pragma once
#include <Arduino.h>
#include "model.h"

#define MAX_HISTORY_RECORDS 24
#define MAX_RECENT_RECORDS  240

/**
 * @brief 初始化持久化存储，并从 NVS 加载已有的 24 小时历史记录
 */
void initStorage();

/**
 * @brief 向环形缓冲区追加一条整点历史记录，并立即同步保存至 NVS
 * @param rec 待保存的环境快照
 */
void appendHourlyRecord(const EnvRecord &rec);

/**
 * @brief 向内存实时高精缓冲区追加一条最新采样 (高频，不写 Flash)
 * @param rec 待保存的环境快照
 */
void appendRecentRecord(const EnvRecord &rec);

/**
 * @brief 获取按时间正序（从旧到新）排列的有效整点历史记录列表
 * @param outArray 输出数组指针，最小容量为 MAX_HISTORY_RECORDS
 * @param maxLen 输出数组最大容量
 * @return uint8_t 实际获取到的记录条数 (0 ~ 24)
 */
uint8_t getHistoryRecords(EnvRecord *outArray, uint8_t maxLen);

/**
 * @brief 获取指定时间窗口 [windowStart, windowEnd] 内的所有有效记录 (合并 NVS整点 + 内存实时采样)
 * @param outArray 输出数组
 * @param maxLen 数组容量
 * @param windowStart 窗口起始时间戳 (秒)
 * @param windowEnd 窗口结束时间戳 (秒)
 * @return uint16_t 实际获取到的点数
 */
uint16_t getRecordsInWindow(EnvRecord *outArray, uint16_t maxLen, uint32_t windowStart, uint32_t windowEnd);

/**
 * @brief 当前已存的历史点数 (0 ~ 24)
 */
uint8_t getHistoryCount();

/**
 * @brief 获取当前系统时间戳 (秒，优先使用真实世界时间，否则使用开机秒数)
 */
uint32_t getSystemTimestamp();

/**
 * @brief 强制将当前内存缓冲区保存至 NVS
 */
void flushStorageToNVS();
