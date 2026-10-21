#pragma once
#include <Arduino.h>

/**
 * @brief 读取 CoreInk 当前电池电压 (V)
 */
float getBatteryVoltage();

/**
 * @brief 读取 CoreInk 电池百分比估算 (0 ~ 100%)
 */
int getBatteryPercentage();
