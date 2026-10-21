#pragma once
#include <Arduino.h>
#include "model.h"

enum ChartMode {
    CHART_MODE_ZOOM = 0,
    CHART_MODE_PAN  = 1
};

enum ActivePage {
    PAGE_DASHBOARD = 0,
    PAGE_AIR_LAB   = 1
};

/**
 * @brief 初始化 E-Ink 显示屏与 200x200 画布 Sprite
 */
void initDashboard();

/**
 * @brief 渲染并刷新完整仪表盘 (Page 0: 核心温湿度 + 历史折线图 + 底部概览)
 */
void renderDashboard(const EnvRecord &current, const char *timeStr, bool fullRefresh = false);

/**
 * @brief 渲染并刷新空气质量与气体深度实验室界面 (Page 1: Static IAQ, bVOC, eCO2, Gas阻抗, 校准状态)
 */
void renderAirLab(const EnvRecord &current, const char *timeStr, bool fullRefresh = false);

/**
 * @brief 仅刷新顶部状态栏 (时间 + 电池)，其余区域不动。用于每分钟的时间更新。
 */
void renderStatusBarOnly(const char *timeStr);

/**
 * @brief 切换折线图模式 (ZOOM <-> PAN) (G38 按键触发)
 */
void chartToggleMode();

/**
 * @brief 按键 G37 (UP) 动作：ZOOM 模式下放大(时间跨度缩小)；PAN 模式下向左平移(看过去)
 */
void chartHandleUp();

/**
 * @brief 按键 G39 (DOWN) 动作：ZOOM 模式下缩小(时间跨度放大)；PAN 模式下向右平移(看最新)
 */
void chartHandleDown();

/**
 * @brief 获取当前图表模式
 */
ChartMode chartGetMode();

/**
 * @brief 切换当前活动显示页面 (G5 / EXT 按键触发)
 */
void toggleActivePage();

/**
 * @brief 获取当前活动显示页面 (PAGE_DASHBOARD 或 PAGE_AIR_LAB)
 */
ActivePage getActivePage();

/**
 * @brief 渲染功耗模式选择悬浮菜单 (底部显示所选模式的简要说明，无按键提示)
 */
void renderPowerMenuOverlay(uint8_t selectedIndex);

