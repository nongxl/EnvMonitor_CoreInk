#pragma once
#include <Arduino.h>

enum PowerMode {
    PWR_MODE_REALTIME = 0,   // 1. 实时模式 (240MHz, 3s采样, ~10小时续航)
    PWR_MODE_BALANCED = 1,   // 2. 均衡模式 (80MHz + 动态Light-Sleep, ~6天续航)
    PWR_MODE_ECO      = 2,   // 3. 长效续航 (10分钟Deep-Sleep + RTC唤醒, ~1.5~2个月续航)
    PWR_MODE_MANUAL   = 3,   // 4. 按需采样 (全时Deep-Sleep, 按键唤醒测1次, 数月+)
    PWR_MODE_COUNT    = 4
};

void initPowerManager();
PowerMode getPowerMode();
void setPowerMode(PowerMode mode);
const char* getPowerModeName(PowerMode mode);

// 选单状态与交互接口
bool isPowerMenuOpen();
void openPowerMenu();
void closePowerMenu();
void powerMenuHandleUp();
void powerMenuHandleDown();
void powerMenuHandleConfirm();
void renderPowerMenu();

// 睡眠调度与进入休眠
void executePowerSleep(bool hasRenderedData = false);

/**
 * @brief 通知电源管理器当前有用户按键活动，刷新活跃窗口防止误休眠
 */
void notifyUserActivity();
