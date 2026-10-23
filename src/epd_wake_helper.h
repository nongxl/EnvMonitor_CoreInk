#pragma once

#include <Arduino.h>
#include <driver/rtc_io.h>

// 访问 M5GFX 底层面板驱动私有成员，消除休眠唤醒强制黑白反转全刷
#define private public
#define protected public
#include <M5CoreInk.h>
#include <lgfx/v1/panel/Panel_GDEW0154D67.hpp>
#include <lgfx/v1/panel/Panel_GDEW0154M09.hpp>
#undef private
#undef protected

/**
 * @brief 在进入 Deep Sleep 前硬件锁存 GPIO 0 (EPD_RST) 为高电平
 * 彻底防止休眠与唤醒期间墨水屏控制器硬件复位，确保芯片内部 SRAM 显存数据完好保留。
 */
inline void epdLockResetPin() {
    rtc_gpio_init(GPIO_NUM_0);
    rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_0, 1);
    rtc_gpio_hold_en(GPIO_NUM_0);
}

/**
 * @brief 驱动总线与面板初始化完成后解除 GPIO 0 硬件锁存
 */
inline void epdUnlockResetPin() {
    rtc_gpio_hold_dis(GPIO_NUM_0);
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
}

/**
 * @brief 配置墨水屏唤醒刷新模式
 * @param fullRefresh 若为 true，执行高质量全刷（消除残影）；若为 false，执行 300ms 极速无闪局部差分刷新。
 */
inline void configureEpdWakeMode(bool fullRefresh) {
    auto panel = M5.M5Ink.getPanel();
    if (!panel) return;

    if (fullRefresh) {
        M5.M5Ink.setEpdMode(epd_mode_t::epd_quality);
    } else {
        // 1. 设置应用层模式为快速局部刷新
        M5.M5Ink.setEpdMode(epd_mode_t::epd_fast);

        // 2. 核心 Hack：若是 GDEW0154D67 (SSD1608)，抹去 init 带来的强制全刷反转标记
        auto pD67 = (lgfx::Panel_GDEW0154D67*)panel;
        pD67->_initialize_seq = false;
        pD67->_need_flip_draw = false;
        pD67->_last_epd_mode = epd_mode_t::epd_fast;

        // 3. 向 SSD1608 控制器写入 0x1C (DISPLAY Mode 2: Differential Partial Refresh without flicker)
        auto bus = pD67->getBus();
        if (bus) {
            pD67->startWrite();
            bus->writeCommand(0x22, 8); // cmd_display_update_control_2
            bus->writeData(0x1C, 8);    // DISPLAY Mode 2 (0x1C: no flick)
            pD67->endWrite();
        }
    }
}
