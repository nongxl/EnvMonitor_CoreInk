#include <Arduino.h>
#include <M5CoreInk.h>

#include "model.h"
#include "time_sync.h"
#include "sensor_bsec.h"
#include "storage.h"
#include "ui_dashboard.h"
#include "battery.h"
#include "power_manager.h"

// 计时与调度变量 (RTC 内存保持，深度休眠唤醒不丢失整点时序)
RTC_DATA_ATTR static int8_t s_lastHour = -1;
RTC_DATA_ATTR static int8_t s_lastMinute = -1;
static uint32_t s_lastHourlySaveMs = 0;
static bool s_firstDataRendered = false;

// 电源键 (G27 - POWER) 状态机变量：双击关机，单击重启
static uint32_t s_lastPwrPressMs = 0;
static uint8_t s_pwrClickCount = 0;
static const uint32_t DOUBLE_CLICK_WINDOW_MS = 380; // 380ms 双击判定窗口

static void getTimeString(char *outStr, size_t maxLen) {
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    M5.rtc.GetTime(&t);
    M5.rtc.GetDate(&d);
    snprintf(outStr, maxLen, "%04d-%02d-%02d %02d:%02d", d.Year, d.Month, d.Date, t.Hours, t.Minutes);
}

/**
 * @brief 彻底断电关机函数
 * @note  墨水屏保留原画面不变；拉低 GPIO 12 彻底切断电池供电，绿色指示灯熄灭。
 */
static void powerOffDevice() {
    Serial.println("\n========================================");
    Serial.println("[Power] Double-click detected on G27 (POWER).");
    Serial.println("[Power] Powering off: Screen content preserved, cutting battery latch.");
    Serial.println("========================================");

    // 1. 保存当前历史数据到 NVS
    flushStorageToNVS();

    // 2. 将墨水屏驱动芯片置入 Deep Sleep，切断驱动 IC 功耗（物理画面永久保持，发挥墨水屏优势）
    M5.M5Ink.deepSleep();
    delay(50);

    // 3. 彻底拉低电源锁存引脚 (GPIO 12)
    // 纯电池供电时，拉低 GPIO 12 的瞬间电源芯片失锁彻底断电，绿色电源指示灯立即熄灭！
    pinMode(POWER_HOLD_PIN, OUTPUT);
    digitalWrite(POWER_HOLD_PIN, LOW);

    // 4. 辅助安全休眠：以防插着 USB-C 导致主板无法硬件断电
    // 开启 G27 (POWER 键) 低电平唤醒，插电时也能通过按键再次唤醒
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PWR_PIN, 0);
    esp_deep_sleep_start();
}

/**
 * @brief 检测电源键 (G27)：双击关机、单击重启
 */
static void checkPowerKey() {
    if (M5.BtnPWR.wasPressed()) {
        uint32_t now = millis();
        if (s_pwrClickCount == 0) {
            s_pwrClickCount = 1;
            s_lastPwrPressMs = now;
        } else if (s_pwrClickCount == 1) {
            if (now - s_lastPwrPressMs <= DOUBLE_CLICK_WINDOW_MS) {
                // 在 380ms 窗口期内按下第 2 次 -> 判定为【双击关机】
                s_pwrClickCount = 0;
                powerOffDevice();
                return;
            } else {
                s_pwrClickCount = 1;
                s_lastPwrPressMs = now;
            }
        }
    }

    // 超过窗口期未按第 2 次 -> 确认判定为【单击重启】
    if (s_pwrClickCount == 1 && (millis() - s_lastPwrPressMs > DOUBLE_CLICK_WINDOW_MS)) {
        s_pwrClickCount = 0;
        Serial.println("\n[Power] Single-click detected on G27 (POWER) -> Restarting ESP32...");
        flushStorageToNVS();
        ESP.restart();
    }
}

static void renderCurrentPage(const EnvRecord &rec, const char *timeStr, bool fullRefresh = false) {
    if (getActivePage() == PAGE_DASHBOARD) {
        renderDashboard(rec, timeStr, fullRefresh);
    } else {
        renderAirLab(rec, timeStr, fullRefresh);
    }
}

void setup() {
    // 1. 硬件初始化 & 供电引脚持续拉高 (GPIO 12)
    M5.begin(true, true, false);
    pinMode(POWER_HOLD_PIN, OUTPUT);
    digitalWrite(POWER_HOLD_PIN, HIGH);
    gpio_hold_dis((gpio_num_t)POWER_HOLD_PIN); // 解除上一次深度睡眠的硬件锁存

    // 彻底熄灭 CoreInk 绿色外设指示灯 (GPIO 10 是低电平点亮，输出 HIGH 熄灭)
    pinMode(LED_EXT_PIN, OUTPUT);
    digitalWrite(LED_EXT_PIN, HIGH);

    // 关键修正：关闭官方库默认的硬件反相，恢复真正的正相白底黑字显示！
    M5.M5Ink.invertDisplay(false);

    Serial.println("\n========================================");
    Serial.println("  CoreInk Humidor Environmental Monitor  ");
    Serial.println("========================================");

    // 检查唤醒原因：区别对待冷启动、定时器唤醒与按键唤醒
    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    bool isTimerWake = (wakeCause == ESP_SLEEP_WAKEUP_TIMER);
    bool isButtonWake = (wakeCause == ESP_SLEEP_WAKEUP_EXT0 || wakeCause == ESP_SLEEP_WAKEUP_EXT1);
    bool isWakeFromSleep = isTimerWake || isButtonWake;

    // 2. 初始化持久化存储 (NVS 恢复整点，RTC 内存恢复高频实时数据)
    initStorage();

    // 3. 初始化 200x200 E-Ink 仪表盘
    initDashboard();

    // 4. Wi-Fi NTP 授时 (仅在冷启动首次开机时进行，休眠唤醒直接使用硬件 RTC，彻底消除等待与电量浪费)
    if (!isWakeFromSleep) {
        renderSplashScreen("Connecting Wi-Fi...");
        syncNtpTime(8000);
        delay(600); // 确保开机画面物理电荷泵驱动彻底沉淀，防止与仪表盘帧冲突
    } else {
        Serial.printf("[RTC Boot] Woke from deep sleep (cause=%d), skipping Wi-Fi NTP.\n", (int)wakeCause);
    }

    // 5. 校验并打印当前 BM8563 硬件 RTC 时间
    RTC_TimeTypeDef initTime;
    RTC_DateTypeDef initDate;
    M5.rtc.GetTime(&initTime);
    M5.rtc.GetDate(&initDate);
    Serial.printf("[RTC Boot Time] %04d-%02d-%02d %02d:%02d:%02d\n",
                  initDate.Year, initDate.Month, initDate.Date,
                  initTime.Hours, initTime.Minutes, initTime.Seconds);

    bool isHourRolledOver = (s_lastHour >= 0 && initTime.Hours != s_lastHour);

    // 6. 初始化 Bosch BSEC 传感器 (Unit ENV-Pro / BME688)
    initBSEC();

    // 7. 同步等待有效读数 (最多 4 秒)，确保开机/唤醒直接显示真实传感器数据
    EnvRecord initialRec;
    bool gotFirst = waitForFirstReading(initialRec, 4000);
    if (gotFirst) {
        s_firstDataRendered = true;
        appendRecentRecord(initialRec);
    } else {
        EnvRecord cached = getLatestEnvRecord();
        if (cached.temp != 0.0f || cached.humi != 0.0f) {
            initialRec = cached;
            gotFirst = true;
        } else if (getHistoryCount() > 0) {
            getLatestHistoryRecord(initialRec);
            gotFirst = true;
        }
    }

    if (isHourRolledOver && gotFirst) {
        EnvRecord recToSave = initialRec;
        recToSave.timestamp = getSystemTimestamp();
        appendHourlyRecord(recToSave);
    }
    s_lastHour = initTime.Hours;
    s_lastMinute = initTime.Minutes;
    s_lastHourlySaveMs = millis();

    char timeStr[32];
    getTimeString(timeStr, sizeof(timeStr));

    // 核心优化：
    // - 冷启动上电：全刷清除残影 (fullRefresh = true)
    // - 周期唤醒 / 按键唤醒：快速局部无闪刷新 (fullRefresh = false)，绝不闪黑白全屏！
    renderCurrentPage(initialRec, timeStr, !isWakeFromSleep);

    // 8. 初始化电源管理器 (读取 NVS 功耗模式并应用对应 CPU 主频与采样率)
    initPowerManager();

    // 如果处于长效续航 (Eco ULP) 或按需采样模式，并在定时器静默周期唤醒下：
    // 数据刷新完毕后，延时等待墨水屏刷屏完成，安然进入下一轮休眠
    if (isTimerWake && (getPowerMode() == PWR_MODE_ECO || getPowerMode() == PWR_MODE_MANUAL)) {
        Serial.println("[Eco] Periodic timer sample rendered smoothly. Returning to Deep Sleep immediately.");
        delay(400);
        executePowerSleep(true);
    } else {
        // 冷启动开机或用户按键唤醒：刷新活跃窗口，支持 30 秒内顺畅翻页与查看
        notifyUserActivity();
    }
}

void loop() {
    M5.update();

    // 活跃窗口感知：用户触碰/拨动/长按任意按键时，持续延长 350ms 全速运作窗口，防止连续操作中途休眠
    if (M5.BtnEXT.wasPressed() || M5.BtnEXT.isPressed() || M5.BtnEXT.wasReleased() ||
        M5.BtnPWR.wasPressed() || M5.BtnPWR.isPressed() || M5.BtnPWR.wasReleased() ||
        M5.BtnMID.wasPressed() || M5.BtnMID.isPressed() || M5.BtnMID.wasReleased() ||
        M5.BtnUP.wasPressed()  || M5.BtnUP.isPressed()  || M5.BtnUP.wasReleased()  ||
        M5.BtnDOWN.wasPressed()|| M5.BtnDOWN.isPressed()|| M5.BtnDOWN.wasReleased()) {
        notifyUserActivity();
    }

    // 核心交互：G27 电源键【双击关机 / 单击重启】
    checkPowerKey();

    // 核心交互：G5 (BtnEXT)
    // - 长按 >= 800ms：弹出功耗模式选择悬浮菜单
    // - 选单开启时短按：取消关闭选单
    // - 选单关闭时短按：切换【主仪表盘】与【空气质量实验室】页面
    static uint32_t s_extPressStart = 0;
    static bool s_extHandledLong = false;
    if (M5.BtnEXT.wasPressed()) {
        s_extPressStart = millis();
        s_extHandledLong = false;
    }
    if (M5.BtnEXT.isPressed()) {
        if (!s_extHandledLong && (millis() - s_extPressStart >= 800)) {
            s_extHandledLong = true;
            if (!isPowerMenuOpen()) {
                openPowerMenu();
            }
        }
    }
    if (M5.BtnEXT.wasReleased()) {
        if (!s_extHandledLong && (millis() - s_extPressStart < 800)) {
            char tStr[32];
            getTimeString(tStr, sizeof(tStr));
            EnvRecord rec = isSensorReady() ? getLatestEnvRecord() : EnvRecord();

            if (isPowerMenuOpen()) {
                closePowerMenu();
                renderCurrentPage(rec, tStr, false);
            } else {
                toggleActivePage();
                Serial.printf("[UI] G5 (EXT) pressed -> Switched page to: %s\n",
                              (getActivePage() == PAGE_DASHBOARD) ? "DASHBOARD" : "AIR_LAB");
                renderCurrentPage(rec, tStr, false);
            }
        }
    }

    // 顶部拨轮 G38 (BtnMID) 状态变量
    static uint32_t s_midPressStart = 0;
    static bool s_midHandledLong = false;

    // 选单开启时的专属导航逻辑 (UP/DOWN 选择，MID 确认)
    if (isPowerMenuOpen()) {
        if (M5.BtnUP.wasPressed()) {
            powerMenuHandleUp();
        }
        if (M5.BtnDOWN.wasPressed()) {
            powerMenuHandleDown();
        }
        if (M5.BtnMID.wasPressed()) {
            powerMenuHandleConfirm();
            s_midPressStart = millis();
            s_midHandledLong = true; // 标记本次按下已由选单确认消费，按键释放时不触发图表切换
            char tStr[32];
            getTimeString(tStr, sizeof(tStr));
            EnvRecord rec = isSensorReady() ? getLatestEnvRecord() : EnvRecord();
            renderCurrentPage(rec, tStr, true);
        }
        delay(20);
        return;
    }

    // 选单未打开时：常规仪表盘滚轮按键交互
    // 顶部拨轮 G38 (BtnMID)：
    // - 在 Mode 4 (On-Demand) 下：长按 G38 (>= 1秒) 触发单次即时采样并全刷上屏
    // - 短按单击切换【缩放 ZOOM / 平移 PAN】操作（注意：关机专属 G27 电源键双击，拨轮绝不误关机！）
    if (M5.BtnMID.wasPressed()) {
        s_midPressStart = millis();
        s_midHandledLong = false;
    }
    if (M5.BtnMID.isPressed()) {
        if (!s_midHandledLong && s_midPressStart > 0) {
            if (getPowerMode() == PWR_MODE_MANUAL && (millis() - s_midPressStart >= 1000)) {
                s_midHandledLong = true;
                Serial.println("[Manual] G38 long-press -> Triggering on-demand sample...");
                EnvRecord manualRec;
                if (waitForFirstReading(manualRec, 3000)) {
                    appendRecentRecord(manualRec);
                    char tStr[32];
                    getTimeString(tStr, sizeof(tStr));
                    renderCurrentPage(manualRec, tStr, false);
                }
            }
        }
    }
    if (M5.BtnMID.wasReleased()) {
        if (!s_midHandledLong && s_midPressStart > 0 && (millis() - s_midPressStart < 1000)) {
            chartToggleMode();
            Serial.printf("[Chart] G38 pressed -> Mode toggled to: %s\n",
                          (chartGetMode() == CHART_MODE_ZOOM) ? "ZOOM" : "PAN");
            char tStr[32];
            getTimeString(tStr, sizeof(tStr));
            EnvRecord rec = isSensorReady() ? getLatestEnvRecord() : EnvRecord();
            renderCurrentPage(rec, tStr, false);
        }
        s_midPressStart = 0;
        s_midHandledLong = false;
    }

    // 按键 G37 (UP)：ZOOM 模式下放大；PAN 模式下向左平移
    if (M5.BtnUP.wasPressed()) {
        chartHandleUp();
        char tStr[32];
        getTimeString(tStr, sizeof(tStr));
        EnvRecord rec = isSensorReady() ? getLatestEnvRecord() : EnvRecord();
        renderCurrentPage(rec, tStr, false);
    }

    // 按键 G39 (DOWN)：ZOOM 模式下缩小；PAN 模式下向右平移
    if (M5.BtnDOWN.wasPressed()) {
        chartHandleDown();
        char tStr[32];
        getTimeString(tStr, sizeof(tStr));
        EnvRecord rec = isSensorReady() ? getLatestEnvRecord() : EnvRecord();
        renderCurrentPage(rec, tStr, false);
    }

    // 后台传感器自愈重试
    checkAndRecoverSensor();

    EnvRecord currentRec;
    bool hasNewData = runBSEC(currentRec);

    // 获取当前 RTC 走时 (年月日 时分)
    RTC_TimeTypeDef curTime;
    M5.rtc.GetTime(&curTime);

    char timeStr[32];
    getTimeString(timeStr, sizeof(timeStr));

    // 新的 BSEC 采样到达时：持续记录至高精实时环形缓冲 (每 10 秒存 1 点)
    static uint32_t s_lastRecentRecordMs = 0;
    if (hasNewData) {
        Serial.printf("[Sensor] T:%.1fC, H:%.1f%%, P:%.0fhPa, IAQ:%.0f, eCO2:%.0fppm\n",
                      currentRec.temp, currentRec.humi, currentRec.press, currentRec.iaq, currentRec.eco2);
        if (s_lastRecentRecordMs == 0 || millis() - s_lastRecentRecordMs >= 10000UL) {
            s_lastRecentRecordMs = millis();
            appendRecentRecord(currentRec);
        }
    }

    // 判断整点周期：当小时发生翻转或者距上次已满 60 分钟 (3600秒)
    bool isHourlyTrigger = false;
    if (s_lastHour >= 0 && curTime.Hours != s_lastHour) {
        isHourlyTrigger = true;
    } else if (millis() - s_lastHourlySaveMs >= 3600000UL) {
        isHourlyTrigger = true;
    }

    if (isHourlyTrigger) {
        Serial.printf("[Schedule] Hour rollover detected (%02d -> %02d). Persisting record...\n",
                      s_lastHour, curTime.Hours);

        if (isSensorReady()) {
            EnvRecord recToSave = getLatestEnvRecord();
            recToSave.timestamp = getSystemTimestamp();
            appendHourlyRecord(recToSave);
            renderCurrentPage(recToSave, timeStr, true);
        } else {
            Serial.println("[Schedule] Warning: Sensor offline, skip hourly record persistence.");
            renderCurrentPage(currentRec, timeStr, true);
        }

        s_lastHour = curTime.Hours;
        s_lastMinute = curTime.Minutes;
        s_lastHourlySaveMs = millis();
    } else if (hasNewData && !s_firstDataRendered) {
        // 开机首包数据就绪：立即刷新上屏
        s_lastMinute = curTime.Minutes;
        renderCurrentPage(currentRec, timeStr, false);
        s_firstDataRendered = true;
    } else if (getPowerMode() == PWR_MODE_REALTIME && hasNewData) {
        // 模式 1 (Realtime 极速模式)：有新采样即时快速刷新 (3秒级)
        s_lastMinute = curTime.Minutes;
        renderCurrentPage(currentRec, timeStr, false);
    } else if (curTime.Minutes != s_lastMinute) {
        // 模式 2 (Balanced 均衡模式) / 模式 3 / 4：
        // 每整分钟随时间跳动整屏同步刷新一次
        s_lastMinute = curTime.Minutes;
        EnvRecord recToRender = isSensorReady() ? getLatestEnvRecord() : currentRec;
        renderCurrentPage(recToRender, timeStr, false);
    }

    // 电源管理与休眠调度 (Light-Sleep / Deep-Sleep / delay)
    executePowerSleep(hasNewData || isHourlyTrigger);
}
