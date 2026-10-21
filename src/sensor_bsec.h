#pragma once
#include <Arduino.h>
#include "model.h"

/**
 * @brief 初始化 Bosch BSEC 算法库及 BME688 传感器 (Grove Port A)
 * @return true 初始化成功，false 传感器未找到或配置失败
 */
bool initBSEC();

/**
 * @brief 在主循环中轮询运行 BSEC
 * @param outRecord 如果产出了新的有效数据则填充并返回 true
 * @return true 有新的传感器采样产出
 */
bool runBSEC(EnvRecord &outRecord);

/**
 * @brief 同步等待第一包有效采样读数 (用于开机即时渲染真实数据)
 * @param outRec 输出环境数据
 * @param timeoutMs 超时时间 (毫秒)，默认 4000ms
 * @return true 成功获取第一包数据
 */
bool waitForFirstReading(EnvRecord &outRec, uint32_t timeoutMs = 4000);

/**
 * @brief 获取内存中最新的一帧传感器数据
 */
EnvRecord getLatestEnvRecord();

/**
 * @brief 传感器是否已成功初始化就绪
 */
bool isSensorReady();

/**
 * @brief 如果传感器未就绪，在后台定期尝试自动重试探测与重新初始化
 * @return true 如果重试成功使传感器恢复
 */
bool checkAndRecoverSensor();

/**
 * @brief 保存/恢复 BSEC 状态至 RTC 慢速内存 (用于 Deep-Sleep 跨周期保持基线)
 */
void saveBsecState();
void loadBsecState();

/**
 * @brief 动态调整 BSEC 采样率 (BSEC_SAMPLE_RATE_LP 3s 或 BSEC_SAMPLE_RATE_ULP 300s)
 */
void setBsecSampleRate(float sampleRate);

