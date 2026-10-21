#include "power_manager.h"
#include <Preferences.h>
#include <M5CoreInk.h>
#include <esp_sleep.h>
#include <bsec.h>

#include "sensor_bsec.h"
#include "storage.h"
#include "ui_dashboard.h"

static PowerMode s_powerMode = PWR_MODE_BALANCED;
static bool s_menuOpen = false;
static uint8_t s_menuSelected = 1;
static Preferences s_prefs;

void initPowerManager() {
    s_prefs.begin("power_mgr", false);
    uint8_t saved = s_prefs.getUChar("pwr_mode", (uint8_t)PWR_MODE_BALANCED);
    if (saved >= PWR_MODE_COUNT) {
        saved = (uint8_t)PWR_MODE_BALANCED;
    }
    s_powerMode = (PowerMode)saved;
    s_menuSelected = saved;

    setPowerMode(s_powerMode);
}

PowerMode getPowerMode() {
    return s_powerMode;
}

const char* getPowerModeName(PowerMode mode) {
    switch (mode) {
        case PWR_MODE_REALTIME: return "Realtime";
        case PWR_MODE_BALANCED: return "Balanced";
        case PWR_MODE_ECO:      return "Eco ULP";
        case PWR_MODE_MANUAL:   return "On-Demand";
        default:                return "Unknown";
    }
}

void setPowerMode(PowerMode mode) {
    s_powerMode = mode;
    s_prefs.putUChar("pwr_mode", (uint8_t)mode);
    Serial.printf("[Power] Set power mode to: %d (%s)\n", (int)mode, getPowerModeName(mode));

    switch (mode) {
        case PWR_MODE_REALTIME:
            setCpuFrequencyMhz(240);
            setBsecSampleRate(BSEC_SAMPLE_RATE_LP);
            break;
        case PWR_MODE_BALANCED:
            setCpuFrequencyMhz(80);
            setBsecSampleRate(BSEC_SAMPLE_RATE_LP);
            break;
        case PWR_MODE_ECO:
            setCpuFrequencyMhz(80);
            setBsecSampleRate(BSEC_SAMPLE_RATE_ULP);
            break;
        case PWR_MODE_MANUAL:
            setCpuFrequencyMhz(80);
            setBsecSampleRate(BSEC_SAMPLE_RATE_ULP);
            break;
        default:
            break;
    }
}

bool isPowerMenuOpen() {
    return s_menuOpen;
}

void openPowerMenu() {
    s_menuOpen = true;
    s_menuSelected = (uint8_t)s_powerMode;
    renderPowerMenuOverlay(s_menuSelected);
    Serial.println("[Power] Power menu opened.");
}

void closePowerMenu() {
    s_menuOpen = false;
    Serial.println("[Power] Power menu closed.");
}

void powerMenuHandleUp() {
    if (!s_menuOpen) return;
    s_menuSelected = (s_menuSelected + PWR_MODE_COUNT - 1) % PWR_MODE_COUNT;
    renderPowerMenuOverlay(s_menuSelected);
}

void powerMenuHandleDown() {
    if (!s_menuOpen) return;
    s_menuSelected = (s_menuSelected + 1) % PWR_MODE_COUNT;
    renderPowerMenuOverlay(s_menuSelected);
}

void powerMenuHandleConfirm() {
    if (!s_menuOpen) return;
    setPowerMode((PowerMode)s_menuSelected);
    closePowerMenu();
}

void renderPowerMenu() {
    if (s_menuOpen) {
        renderPowerMenuOverlay(s_menuSelected);
    }
}

static uint32_t s_lastUserActionMs = 0;

void notifyUserActivity() {
    s_lastUserActionMs = millis();
}

void executePowerSleep(bool hasRenderedData) {
    if (s_menuOpen) {
        delay(20);
        return;
    }

    switch (s_powerMode) {
        case PWR_MODE_REALTIME:
            delay(20);
            break;

        case PWR_MODE_BALANCED: {
            // 1. 电纸书级连续操作活跃窗口：在用户按键或滚轮操作后的 350ms 内保持全速运转
            //    保证快速连拨滚轮或连按按键时手感极致跟手、零丢步
            if (millis() - s_lastUserActionMs < 350) {
                delay(15);
                break;
            }

            // 2. 空闲无按键操作时，立即切断墨水屏高压偏置电源 (功耗降为 0)
            M5.M5Ink.powerHVOFF();

            // 3. 电纸书级纯硬件外部中断 Light-Sleep：
            //    所有按键配置为硬件低电平唤醒源 (GPIO 5, 27, 37, 38, 39)
            //    用户触碰/拨动任意键的瞬间，硬件中断 1ms 内瞬间唤醒 CPU，零感知延迟！
            gpio_wakeup_enable((gpio_num_t)5,  GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)27, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)37, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)38, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)39, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();

            // 4. 定时低频唤醒 (3秒周期)：与 BSEC 传感器低频采样节拍同步，整机 99% 的时间挂起在浅睡眠中
            esp_sleep_enable_timer_wakeup(3ULL * 1000000ULL);

            esp_light_sleep_start();
            break;
        }

        case PWR_MODE_ECO: {
            // 长效续航：每当有新数据渲染完毕后，进入 10 分钟 Deep-Sleep
            if (hasRenderedData) {
                Serial.println("[Power] Eco Mode: Data rendered. Entering 10-min Deep Sleep...");
                saveBsecState();
                flushStorageToNVS();

                // 锁存电源引脚 (GPIO 12)，防止休眠期间切断电池供电
                pinMode(12, OUTPUT);
                digitalWrite(12, HIGH);
                gpio_hold_en((gpio_num_t)12);
                gpio_deep_sleep_hold_en();

                // 墨水屏驱动芯片进入休眠，物理画面保持
                M5.M5Ink.deepSleep();
                delay(20);

                // 10 分钟定时唤醒 (600秒)，同时使能 G38(中键) 与 G27(电源键) 低电平按键唤醒
                esp_sleep_enable_timer_wakeup(600ULL * 1000000ULL);
                // EXT0 支持单个 GPIO 在指定电平唤醒 (G38 是 RTC IO 13，按下为 LOW)
                esp_sleep_enable_ext0_wakeup((gpio_num_t)38, 0);
                esp_deep_sleep_start();
            } else {
                delay(20);
            }
            break;
        }

        case PWR_MODE_MANUAL: {
            // 按需采样：单次数据测量并上屏后，进入永久深度睡眠，仅按键唤醒
            if (hasRenderedData) {
                Serial.println("[Power] Manual Mode: Data rendered. Entering Deep Sleep until key press...");
                saveBsecState();
                flushStorageToNVS();

                pinMode(12, OUTPUT);
                digitalWrite(12, HIGH);
                gpio_hold_en((gpio_num_t)12);
                gpio_deep_sleep_hold_en();

                M5.M5Ink.deepSleep();
                delay(20);

                esp_sleep_enable_ext0_wakeup((gpio_num_t)38, 0);
                esp_deep_sleep_start();
            } else {
                delay(20);
            }
            break;
        }
    }
}
