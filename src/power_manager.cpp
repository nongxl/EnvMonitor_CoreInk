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
            setBsecSampleRate(BSEC_SAMPLE_RATE_LP);
            break;
        case PWR_MODE_MANUAL:
            setCpuFrequencyMhz(80);
            setBsecSampleRate(BSEC_SAMPLE_RATE_LP);
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
            // 1. 电纸书级连续操作活跃窗口：在用户按键或滚轮操作后的 400ms 内保持全速运转
            //    保证快速连拨滚轮或连按按键时手感极致跟手、零丢步
            if (millis() - s_lastUserActionMs < 400) {
                delay(15);
                break;
            }

            // 2. 电纸书级纯硬件外部中断 Light-Sleep：
            //    所有按键配置为硬件低电平唤醒源 (GPIO 5, 27, 37, 38, 39)
            //    用户触碰/拨动任意键的瞬间，硬件中断 1ms 内瞬间唤醒 CPU，零感知延迟！
            gpio_wakeup_enable((gpio_num_t)5,  GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)27, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)37, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)38, GPIO_INTR_LOW_LEVEL);
            gpio_wakeup_enable((gpio_num_t)39, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();

            // 3. 定时低频唤醒 (1分钟/60秒周期)：与 1 分钟采样节拍同步，整机 99% 的时间挂起在浅睡眠中
            esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);

            esp_light_sleep_start();
            break;
        }

        case PWR_MODE_ECO: {
            // 用户操作活跃期内 (按键/旋钮操作后的 30 秒内) 必须保持清醒响应操作！
            if (millis() - s_lastUserActionMs < 30000) {
                delay(20);
                break;
            }

            // 无操作超过 30 秒，立即进入 10 分钟 Deep-Sleep
            Serial.println("[Power] Eco Mode: Entering 10-min Deep Sleep...");
            saveBsecState();
            flushStorageToNVS();

            // 标记休眠 zZ 徽标并快速局部刷新顶部状态栏
            showSleepIndicator(true);

            // 等待墨水屏最后一次刷屏动作物理完成，彻底防止画面冻结
            delay(400);

            // 彻底关闭外设，熄灭绿色指示灯 (GPIO 10 是低电平点亮，输出 HIGH 熄灭)
            pinMode(10, OUTPUT);
            digitalWrite(10, HIGH);
            M5.M5Ink.deepSleep();

            // 锁存电源引脚 (GPIO 12)，防止休眠期间切断电池供电
            pinMode(12, OUTPUT);
            digitalWrite(12, HIGH);
            gpio_hold_en((gpio_num_t)12);
            gpio_deep_sleep_hold_en();
            delay(20);

            // 10 分钟定时唤醒 (600秒)
            esp_sleep_enable_timer_wakeup(600ULL * 1000000ULL);
            // G38 中键按键唤醒 (RTC IO 2，低电平触发)
            esp_sleep_enable_ext0_wakeup((gpio_num_t)38, 0);
            esp_deep_sleep_start();
            break;
        }

        case PWR_MODE_MANUAL: {
            // 用户操作活跃期内 (按键/旋钮操作后的 30 秒内) 必须保持清醒响应操作！
            if (millis() - s_lastUserActionMs < 30000) {
                delay(20);
                break;
            }

            Serial.println("[Power] Manual Mode: Entering Deep Sleep until key press...");
            saveBsecState();
            flushStorageToNVS();

            // 标记休眠 zZ 徽标并快速局部刷新顶部状态栏
            showSleepIndicator(true);

            // 等待墨水屏最后一次刷屏动作物理完成
            delay(400);

            pinMode(10, OUTPUT);
            digitalWrite(10, HIGH);
            M5.M5Ink.deepSleep();

            pinMode(12, OUTPUT);
            digitalWrite(12, HIGH);
            gpio_hold_en((gpio_num_t)12);
            gpio_deep_sleep_hold_en();
            delay(20);

            esp_sleep_enable_ext0_wakeup((gpio_num_t)38, 0);
            esp_deep_sleep_start();
            break;
        }
    }
}
