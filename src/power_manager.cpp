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
            // 均衡模式：动态 Light-Sleep (按键低电平即时唤醒，或最多休眠 200ms 后轮询)
            gpio_wakeup_enable((gpio_num_t)5, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)27, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)37, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)38, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)39, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
            esp_sleep_enable_timer_wakeup(200ULL * 1000ULL); // 200ms
            esp_light_sleep_start();
            break;
        }

        case PWR_MODE_ECO: {
            // 长效续航：每当有新数据渲染完毕后，进入 5 分钟 Deep-Sleep
            if (hasRenderedData) {
                Serial.println("[Power] Eco Mode: Data rendered. Entering 5-min Deep Sleep...");
                saveBsecState();
                flushStorageToNVS();

                // 锁存电源引脚 (GPIO 12)，防止休眠期间切断供电
                pinMode(12, OUTPUT);
                digitalWrite(12, HIGH);
                gpio_hold_en((gpio_num_t)12);
                gpio_deep_sleep_hold_en();

                M5.M5Ink.deepSleep();
                delay(20);

                // 5 分钟定时唤醒，或者按键 G27/G38 唤醒
                esp_sleep_enable_timer_wakeup(300ULL * 1000000ULL);
                esp_sleep_enable_ext1_wakeup((1ULL << 27) | (1ULL << 38), ESP_EXT1_WAKEUP_ALL_LOW);
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

                esp_sleep_enable_ext1_wakeup((1ULL << 27) | (1ULL << 38), ESP_EXT1_WAKEUP_ALL_LOW);
                esp_deep_sleep_start();
            } else {
                delay(20);
            }
            break;
        }
    }
}
