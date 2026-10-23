#include "ui_dashboard.h"
#include <M5CoreInk.h>
#include "storage.h"
#include "battery.h"
#include "sensor_bsec.h"
#include "img_churchill.h"
#include "epd_wake_helper.h"

static Ink_Sprite s_sprite(&M5.M5Ink);
static bool s_spriteCreated = false;

struct ZoomLevel {
    const char *name;
    uint32_t durationSec;
    uint32_t panStepSec;
};

static const ZoomLevel ZOOM_LEVELS[] = {
    {"5m",     300,    30},  // 5 分钟视窗 (平移步长 30 秒)
    {"10m",    600,    60},  // 10 分钟视窗 (平移步长 1 分钟)
    {"1h",    3600,   300},  // 1 小时视窗 (平移步长 5 分钟)
    {"6h",   21600,  1800},  // 6 小时视窗 (平移步长 30 分钟)
    {"24h",  86400,  3600},  // 24 小时视窗 (平移步长 1 小时)
    {"2d",  172800, 14400},  // 2 天视窗 (平移步长 4 小时)
    {"5d",  432000, 43200},  // 5 天视窗 (平移步长 12 小时)
    {"7d",  604800, 86400}   // 7 天视窗 (平移步长 1 天 / 24 小时)
};
static const int ZOOM_COUNT = 8;
RTC_DATA_ATTR static int s_zoomIdx = 1; // 默认 10m，开机立即可见细粒度动态曲线
RTC_DATA_ATTR static ChartMode s_chartMode = CHART_MODE_ZOOM;
RTC_DATA_ATTR static uint32_t s_panOffsetSec = 0;
static const uint32_t MAX_PAN_OFFSET_SEC = 604800; // 最多平移 7 天

void chartToggleMode() {
    if (s_chartMode == CHART_MODE_ZOOM) {
        s_chartMode = CHART_MODE_PAN;
    } else {
        s_chartMode = CHART_MODE_ZOOM;
    }
}

void chartHandleUp() {
    if (s_chartMode == CHART_MODE_ZOOM) {
        // G37 (UP) in ZOOM: 放大时间轴 (7d -> 5d -> 2d -> 24h -> 6h -> 1h -> 10m -> 5m)
        if (s_zoomIdx > 0) {
            s_zoomIdx--;
            s_panOffsetSec = 0; // 缩放时重置平移
        }
    } else {
        // G37 (UP) in PAN: 向左平移 (查看过去的时刻)
        s_panOffsetSec += ZOOM_LEVELS[s_zoomIdx].panStepSec;
        if (s_panOffsetSec > MAX_PAN_OFFSET_SEC) {
            s_panOffsetSec = MAX_PAN_OFFSET_SEC;
        }
    }
}

void chartHandleDown() {
    if (s_chartMode == CHART_MODE_ZOOM) {
        // G39 (DOWN) in ZOOM: 缩小时间轴 (10m -> 1h -> 6h -> 24h)
        if (s_zoomIdx < ZOOM_COUNT - 1) {
            s_zoomIdx++;
            s_panOffsetSec = 0; // 缩放时重置平移
        }
    } else {
        // G39 (DOWN) in PAN: 向右平移 (查看靠近现在的时刻，直到 Now)
        if (s_panOffsetSec >= ZOOM_LEVELS[s_zoomIdx].panStepSec) {
            s_panOffsetSec -= ZOOM_LEVELS[s_zoomIdx].panStepSec;
        } else {
            s_panOffsetSec = 0;
        }
    }
}

ChartMode chartGetMode() {
    return s_chartMode;
}

RTC_DATA_ATTR static ActivePage s_activePage = PAGE_DASHBOARD;

void toggleActivePage() {
    if (s_activePage == PAGE_DASHBOARD) {
        s_activePage = PAGE_AIR_LAB;
    } else {
        s_activePage = PAGE_DASHBOARD;
    }
}

ActivePage getActivePage() {
    return s_activePage;
}

// 绘制粗线 (2px 黑线)
static void drawThickLine(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
    if (thickness <= 1) {
        s_sprite.drawLine(x0, y0, x1, y1, color);
        return;
    }
    s_sprite.drawLine(x0, y0, x1, y1, color);
    if (abs(y1 - y0) < abs(x1 - x0)) {
        s_sprite.drawLine(x0, y0 + 1, x1, y1 + 1, color);
    } else {
        s_sprite.drawLine(x0 + 1, y0, x1 + 1, y1, color);
    }
}

// 绘制水平虚线 (1px 黑虚线)
static void drawDashedHLine(int x, int y, int w, int onLen, int offLen, uint16_t color) {
    int cycle = onLen + offLen;
    for (int i = 0; i < w; ++i) {
        if ((i % cycle) < onLen) {
            s_sprite.drawPixel(x + i, y, color);
        }
    }
}

// 绘制垂直虚线 (1px 黑虚线)
static void drawDashedVLine(int x, int y, int h, int onLen, int offLen, uint16_t color) {
    int cycle = onLen + offLen;
    for (int i = 0; i < h; ++i) {
        if ((i % cycle) < onLen) {
            s_sprite.drawPixel(x, y + i, color);
        }
    }
}

// 绘制两点间虚线 (1px 黑虚线)
static void drawDashedLine(int x0, int y0, int x1, int y1, int onLen, int offLen, uint16_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int cycle = onLen + offLen;
    int step = 0;

    while (true) {
        if ((step % cycle) < onLen) {
            s_sprite.drawPixel(x0, y0, color);
        }
        step++;

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void initDashboard() {
    Serial.println("[UI] Initializing 200x200 E-Ink Dashboard...");
    if (!s_spriteCreated) {
        s_sprite.creatSprite(0, 0, 200, 200);
        s_spriteCreated = true;
    }
    s_sprite.clear();
}

RTC_DATA_ATTR static bool s_isSleeping = false;

void setSleepIndicator(bool sleeping) {
    s_isSleeping = sleeping;
}

bool getSleepIndicator() {
    return s_isSleeping;
}

void showSleepIndicator(bool sleeping) {
    s_isSleeping = sleeping;
    char timeStr[32];
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    M5.rtc.GetTime(&t);
    M5.rtc.GetDate(&d);
    snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d", d.Year, d.Month, d.Date, t.Hours, t.Minutes);
    renderStatusBarOnly(timeStr);
}

// 顶部状态栏 (Y: 0 ~ 16 px)：左侧年月日时分，右侧电池(电量数字在图标内)
static void drawStatusBar(const char *timeStr) {
    s_sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    s_sprite.fillRect(0, 0, 200, 16, TFT_WHITE);
    s_sprite.setTextSize(1);
    const char *actualTimeStr = timeStr ? timeStr : "----/--/-- --:--";
    s_sprite.drawString(2, 2, actualTimeStr);

    // 若处于 Deep Sleep 休眠状态，在时间右侧显示黑色 zZ 标记 (固定位于 X:143，与时间文字及电池保持黄金留白)
    if (s_isSleeping) {
        s_sprite.drawString(143, 2, "zZ");
    }

    int batPct = getBatteryPercentage();
    char batPctStr[16];
    snprintf(batPctStr, sizeof(batPctStr), "%d", batPct); // 纯数字无百分号

    // 电池图标：整体向左移避开物理黑边 (右端留 10px 边距，主体 X:164~190，端子 190~192)
    const int batW = 28;
    const int batH = 12;
    const int batX = 192 - batW; // 164
    const int batY = 2;          // 上下各留 2px 优雅呼吸间距

    // 先用透明背景绘制文字，上下各留 2px 间隙绝不遮挡边线
    s_sprite.setTextColor(TFT_BLACK);
    int txtW = s_sprite.textWidth(batPctStr);
    int txtX = batX + (batW - txtW) / 2;
    int txtY = batY + 1; // Y: 4 (文字高 8px，占据 Y:4~12，上下各有 2px 留白)
    s_sprite.drawString(txtX-3, txtY-1, batPctStr);

    // 后绘制电池外框与端子，确保线型 100% 连续锐利不被抹平
    s_sprite.drawRect(batX, batY, batW, batH, TFT_BLACK);
    s_sprite.fillRect(batX + batW, batY + 3, 2, 6, TFT_BLACK); // 端子居中高 6px (Y: 5~11)

    // 状态栏底部分割细线
    s_sprite.drawFastHLine(0, 16, 200, TFT_BLACK);
}

void renderStatusBarOnly(const char *timeStr) {
    if (!s_spriteCreated) {
        return;
    }
    // 只重绘画布顶部条带，其余区域沿用上一帧
    drawStatusBar(timeStr);

    // 通过裁剪区域只把顶部 0~17px 推送到屏幕，其余区域不刷新
    M5.M5Ink.setEpdMode(epd_mode_t::epd_fast);
    M5.M5Ink.setClipRect(0, 0, 200, 18);
    s_sprite.pushSprite();
    M5.M5Ink.clearClipRect();
}

void renderDashboard(const EnvRecord &current, const char *timeStr, bool fullRefresh) {
    if (!s_spriteCreated) {
        initDashboard();
    }

    // 1. 全屏纯白底色，前景色纯黑 (呈现正相极佳的白底黑字纸质质感)
    s_sprite.fillScreen(TFT_WHITE);
    s_sprite.setTextColor(TFT_BLACK, TFT_WHITE);

    // 1. 顶部状态栏 (时间 + 电池)
    drawStatusBar(timeStr);

    // ==========================================
    // 2. Primary Metrics (Y: 18 ~ 46 px) 温湿度左右并排大字
    // ==========================================
    bool hasValidData = isSensorReady() && (current.temp != 0.0f || current.humi != 0.0f);

    s_sprite.setTextSize(2);
    char tempNumBuf[16];
    char humiStr[16];

    if (hasValidData) {
        snprintf(tempNumBuf, sizeof(tempNumBuf), "%.1f", current.temp);
        snprintf(humiStr, sizeof(humiStr), "%.1f%%", current.humi);
    } else {
        snprintf(tempNumBuf, sizeof(tempNumBuf), "--.-");
        snprintf(humiStr, sizeof(humiStr), "--.-%%");
    }

    // 2.1 左半区：实时温度数字 + 精准摄氏度小圆圈 "°C"
    int numX = 3;
    s_sprite.drawString(numX, 22, tempNumBuf);

    // 采用与 drawString 一致的 AsciiFont8x16 字体计算真实宽度 (size 2 下每字符 16px)
    int numWidth = s_sprite.textWidth(tempNumBuf, &fonts::AsciiFont8x16);
    if (numWidth <= 0) {
        numWidth = strlen(tempNumBuf) * 16;
    }

    // 摄氏度符号整体向右平移，与数字末尾保持舒适间隙，彻底消除重叠
    int circleX = numX + numWidth + 7;
    int cX = circleX + 7;

    // 绘制加粗度数空心圆圈 (外径 3px，内径 2px 双层圆环加粗) 与大写 C
    s_sprite.drawCircle(circleX, 25, 2, TFT_BLACK);
    s_sprite.drawCircle(circleX, 25, 3, TFT_BLACK);
    s_sprite.drawString(cX, 22, "C");

    // 左右中间竖向细分割线
    s_sprite.drawFastVLine(100, 18, 28, TFT_BLACK);

    // 2.2 右半区：实时湿度大字
    s_sprite.drawString(106, 22, humiStr);

    // 主数值底部分割细线
    s_sprite.drawFastHLine(0, 48, 200, TFT_BLACK);

    // ==========================================
    // 3. Center Dual-Curve Chart (Y: 48 ~ 146 px) 历史趋势 (纯白底黑线，支持缩放/平移)
    // ==========================================
    // 去除纵轴单位充分拓宽图表至 150px：左侧实线温度轴 (-20~40)，右侧虚线湿度轴 (30~90)
    const int chartLeft = 26;
    const int chartRight = 176;
    const int chartTop = 52;
    const int chartBottom = 128; // 压缩折线图高度，为下方时间刻度文字留出充足呼吸空间
    const int chartW = chartRight - chartLeft; // 150 px
    const int chartH = chartBottom - chartTop; // 76 px

    // 坐标轴框线 (左 Y 轴实线、右 Y 轴虚线呼应湿度、底 X 轴实线)
    s_sprite.drawFastVLine(chartLeft, chartTop, chartH, TFT_BLACK);
    drawDashedVLine(chartRight, chartTop, chartH, 2, 2, TFT_BLACK);
    s_sprite.drawFastHLine(chartLeft, chartBottom, chartW, TFT_BLACK);

    // 标尺设定：双刻度大跨度全天候模式 (跨度均为 60)
    // 温度：-20 ~ 40 (跨度 60) -> 对应左 Y 轴 (实线)
    // 湿度： 30 ~ 90 (跨度 60) -> 对应右 Y 轴 (虚线)
    const float tempMin = -20.0f, tempMax = 40.0f;
    const float humiMin = 30.0f, humiMax = 90.0f;

    // 1. 获取当前时间与视窗跨度 [windowStart, windowEnd]
    uint32_t nowSec = getSystemTimestamp();
    uint32_t windowEnd = (nowSec >= s_panOffsetSec) ? (nowSec - s_panOffsetSec) : nowSec;
    uint32_t windowSpan = ZOOM_LEVELS[s_zoomIdx].durationSec;
    uint32_t windowStart = (windowEnd >= windowSpan) ? (windowEnd - windowSpan) : 0;

    auto mapHumiY = [&](float h) -> int {
        if (h < humiMin) h = humiMin;
        if (h > humiMax) h = humiMax;
        return chartBottom - (int)((h - humiMin) / (humiMax - humiMin) * chartH);
    };

    auto mapTempY = [&](float t) -> int {
        if (t < tempMin) t = tempMin;
        if (t > tempMax) t = tempMax;
        return chartBottom - (int)((t - tempMin) / (tempMax - tempMin) * chartH);
    };

    auto mapTimeX = [&](uint32_t ts) -> int {
        if (ts <= windowStart) return chartLeft;
        if (ts >= windowEnd) return chartRight;
        return chartLeft + (int)((float)(ts - windowStart) / (float)(windowEnd - windowStart) * chartW);
    };

    // 辅助 lambda：绘制左侧纯数字温度刻度 (无单位，右对齐于 chartLeft - 2，做非负截断防裁剪)
    auto drawLeftTempTick = [&](int y, const char *numStr) {
        s_sprite.setTextSize(1);
        int nw = s_sprite.textWidth(numStr);
        if (nw <= 0) nw = strlen(numStr) * 6;
        int numX = (chartLeft - 2) - nw;
        if (numX < 0) numX = 0;
        s_sprite.drawString(numX, y, numStr);
    };

    // 2. 获取视窗内数据 (高频采样 + 历史整点，静态缓冲区支持至 7 天 256 点)
    static EnvRecord s_chartPoints[256];
    uint16_t count = getRecordsInWindow(s_chartPoints, 256, windowStart, windowEnd);

    // 若视窗内暂无历史点，且位于当前最新视窗，则以当前读数作为首个数据点
    if (count == 0 && hasValidData && s_panOffsetSec == 0) {
        s_chartPoints[0] = current;
        s_chartPoints[0].timestamp = nowSec;
        count = 1;
    } else if (count > 0 && hasValidData && s_panOffsetSec == 0) {
        // 当前视窗为实时模式时，确保折线末端严格对齐当前实时测得的温湿度数值
        s_chartPoints[count - 1].temp = current.temp;
        s_chartPoints[count - 1].humi = current.humi;
        s_chartPoints[count - 1].press = current.press;
    }

    // 2. 统计当前视窗内线条的最高与最低点 (用于纵轴中间动态刻度展示)
    float curMinTemp = 999.0f, curMaxTemp = -999.0f;
    float curMinHumi = 999.0f, curMaxHumi = -999.0f;
    for (uint16_t i = 0; i < count; ++i) {
        float t = s_chartPoints[i].temp;
        float h = s_chartPoints[i].humi;
        if (t < curMinTemp) curMinTemp = t;
        if (t > curMaxTemp) curMaxTemp = t;
        if (h < curMinHumi) curMinHumi = h;
        if (h > curMaxHumi) curMaxHumi = h;
    }

    // 3. 左侧温度轴刻度：两端标出量程 (40 和 -20)，中间显示折线最高与最低点
    drawLeftTempTick(chartTop - 2, "40");
    drawLeftTempTick(chartBottom - 8, "-20");

    if (count > 0 && curMaxTemp > -900.0f) {
        char maxTStr[16], minTStr[16];
        if (curMaxTemp - curMinTemp < 3.0f) {
            snprintf(maxTStr, sizeof(maxTStr), "%.1f", curMaxTemp);
            snprintf(minTStr, sizeof(minTStr), "%.1f", curMinTemp);
        } else {
            snprintf(maxTStr, sizeof(maxTStr), "%.0f", curMaxTemp);
            snprintf(minTStr, sizeof(minTStr), "%.0f", curMinTemp);
        }

        int yTMax = mapTempY(curMaxTemp);
        int yTMin = mapTempY(curMinTemp);

        // 避免与顶部量程 40 和底部量程 -20 发生文字重叠
        if (yTMax < chartTop + 9) yTMax = chartTop + 9;
        if (yTMin > chartBottom - 16) yTMin = chartBottom - 16;

        if (yTMin - yTMax < 9) {
            // 最高温与最低温重合或极近，显示单个刻度
            s_sprite.drawFastHLine(chartLeft - 3, yTMax, 3, TFT_BLACK);
            drawLeftTempTick(yTMax - 4, maxTStr);
        } else {
            s_sprite.drawFastHLine(chartLeft - 3, yTMax, 3, TFT_BLACK);
            drawLeftTempTick(yTMax - 4, maxTStr);

            s_sprite.drawFastHLine(chartLeft - 3, yTMin, 3, TFT_BLACK);
            drawLeftTempTick(yTMin - 4, minTStr);
        }
    }

    // 4. 右侧湿度轴刻度：两端标出量程 (90 和 30)，中间显示折线最高与最低点
    s_sprite.setTextSize(1);
    s_sprite.drawString(chartRight + 4, chartTop - 2, "90");
    s_sprite.drawString(chartRight + 4, chartBottom - 8, "30");

    if (count > 0 && curMaxHumi > -900.0f) {
        char maxHStr[16], minHStr[16];
        snprintf(maxHStr, sizeof(maxHStr), "%.0f", curMaxHumi);
        snprintf(minHStr, sizeof(minHStr), "%.0f", curMinHumi);

        int yHMax = mapHumiY(curMaxHumi);
        int yHMin = mapHumiY(curMinHumi);

        // 避免与顶部量程 90 和底部量程 30 发生文字重叠
        if (yHMax < chartTop + 9) yHMax = chartTop + 9;
        if (yHMin > chartBottom - 16) yHMin = chartBottom - 16;

        if (yHMin - yHMax < 9) {
            // 最高与最低湿度重合或极近，显示单个刻度
            s_sprite.drawFastHLine(chartRight, yHMax, 3, TFT_BLACK);
            s_sprite.drawString(chartRight + 4, yHMax - 4, maxHStr);
        } else {
            s_sprite.drawFastHLine(chartRight, yHMax, 3, TFT_BLACK);
            s_sprite.drawString(chartRight + 4, yHMax - 4, maxHStr);

            s_sprite.drawFastHLine(chartRight, yHMin, 3, TFT_BLACK);
            s_sprite.drawString(chartRight + 4, yHMin - 4, minHStr);
        }
    }

    // 右上角图例 (黑字白底：T- 温度实线，H.. 湿度虚线)
    s_sprite.drawString(chartRight - 48, chartTop + 2, "T- H..");

    // 模式与视窗状态标签 (移动到横坐标轴中间下方展示)
    char modeTag[32];
    if (s_chartMode == CHART_MODE_ZOOM) {
        snprintf(modeTag, sizeof(modeTag), "[ZOOM:%s]", ZOOM_LEVELS[s_zoomIdx].name);
    } else {
        if (s_panOffsetSec == 0) {
            snprintf(modeTag, sizeof(modeTag), "[PAN:%s]", ZOOM_LEVELS[s_zoomIdx].name);
        } else if (s_panOffsetSec < 3600) {
            snprintf(modeTag, sizeof(modeTag), "[PAN:-%um]", (unsigned)(s_panOffsetSec / 60));
        } else if (s_panOffsetSec < 86400) {
            snprintf(modeTag, sizeof(modeTag), "[PAN:-%.1fh]", (float)s_panOffsetSec / 3600.0f);
        } else {
            snprintf(modeTag, sizeof(modeTag), "[PAN:-%.1fd]", (float)s_panOffsetSec / 86400.0f);
        }
    }

    // 底部时间刻度与居中模式控件 (位于 X 轴下方 Y: 131)
    char leftTimeStr[16];
    char rightTimeStr[16];
    if (s_panOffsetSec == 0) {
        snprintf(leftTimeStr, sizeof(leftTimeStr), "-%s", ZOOM_LEVELS[s_zoomIdx].name);
        snprintf(rightTimeStr, sizeof(rightTimeStr), "Now");
    } else {
        snprintf(leftTimeStr, sizeof(leftTimeStr), "Past");
        if (s_panOffsetSec < 3600) {
            snprintf(rightTimeStr, sizeof(rightTimeStr), "-%um", (unsigned)(s_panOffsetSec / 60));
        } else if (s_panOffsetSec < 86400) {
            snprintf(rightTimeStr, sizeof(rightTimeStr), "-%.1fh", (float)s_panOffsetSec / 3600.0f);
        } else {
            snprintf(rightTimeStr, sizeof(rightTimeStr), "-%.1fd", (float)s_panOffsetSec / 86400.0f);
        }
    }
    // 左：起始时间
    s_sprite.drawString(chartLeft, chartBottom + 3, leftTimeStr);

    // 中：PAN / ZOOM 状态控件居中显示在横坐标下方
    int tagW = s_sprite.textWidth(modeTag, &fonts::AsciiFont8x16);
    if (tagW <= 0) tagW = strlen(modeTag) * 8;
    int tagX = (chartLeft + chartRight - tagW) / 2;
    s_sprite.drawString(tagX, chartBottom + 3, modeTag);

    // 右：截止时间 (如 Now)
    int rtW = s_sprite.textWidth(rightTimeStr, &fonts::AsciiFont8x16);
    if (rtW <= 0) rtW = strlen(rightTimeStr) * 8;
    s_sprite.drawString(chartRight - rtW, chartBottom + 3, rightTimeStr);

    // 绘制温湿度连续折线：当只有 1 个点时绘制水平参考线；多点时绘制连续折线
    if (count == 1) {
        int yH = mapHumiY(s_chartPoints[0].humi);
        int yT = mapTempY(s_chartPoints[0].temp);
        s_sprite.drawFastHLine(chartLeft, yT, chartW, TFT_BLACK);
        drawDashedLine(chartLeft, yH, chartRight, yH, 3, 2, TFT_BLACK);
    } else {
        for (int i = 0; i < count; ++i) {
            int x = mapTimeX(s_chartPoints[i].timestamp);
            int yH = mapHumiY(s_chartPoints[i].humi);
            int yT = mapTempY(s_chartPoints[i].temp);

            if (i > 0) {
                int prevX = mapTimeX(s_chartPoints[i - 1].timestamp);
                int prevYH = mapHumiY(s_chartPoints[i - 1].humi);
                int prevYT = mapTempY(s_chartPoints[i - 1].temp);

                // 温度：黑色粗实线 (2px)
                drawThickLine(prevX, prevYT, x, yT, 2, TFT_BLACK);

                // 湿度：黑色细虚线 (1px)
                drawDashedLine(prevX, prevYH, x, yH, 3, 2, TFT_BLACK);
            }
        }
    }

    // ==========================================
    // 4. Bottom Multi-Gauge Area (Y: 148 ~ 198 px) 底部仪表区
    // ==========================================
    s_sprite.drawFastHLine(0, 148, 200, TFT_BLACK);
    s_sprite.drawFastVLine(100, 150, 48, TFT_BLACK);

    // 4.1 左半区：实时气压 (第一行) 与 海拔高度 (第二行大字)
    if (hasValidData) {
        char pressStr[24];
        snprintf(pressStr, sizeof(pressStr), "P:%.1fhPa", current.press);
        s_sprite.setTextSize(1);
        s_sprite.drawString(4, 153, pressStr);

        // 国际标准大气模型压高公式: h = 44330 * (1 - (P / 1013.25)^0.1903)
        float altitude = 0.0f;
        if (current.press > 0.0f) {
            altitude = 44330.0f * (1.0f - powf(current.press / 1013.25f, 0.190294957f));
        }

        char altStr[16];
        snprintf(altStr, sizeof(altStr), "%.0f m", altitude);
        s_sprite.setTextSize(2);
        s_sprite.drawString(6, 170, altStr);
    } else {
        s_sprite.setTextSize(1);
        s_sprite.drawString(4, 153, "P:--hPa");

        s_sprite.setTextSize(2);
        s_sprite.drawString(6, 170, "-- m");
    }

    // 4.2 右半区：IAQ 与 PPM 仪表
    s_sprite.setTextSize(1);
    if (hasValidData) {
        char iaqPpmStr[24];
        snprintf(iaqPpmStr, sizeof(iaqPpmStr), "IAQ:%.0f %.0fppm", current.iaq, current.eco2);
        s_sprite.drawString(104, 153, iaqPpmStr);

        // 3 段式状态条 (宽 26, 间隙 4, 高 12)
        const int segW = 26;
        const int segH = 12;
        const int segY = 170;

        int xSeg1 = 104;
        int xSeg2 = xSeg1 + segW + 4; // 134
        int xSeg3 = xSeg2 + segW + 4; // 164

        s_sprite.drawRect(xSeg1, segY, segW, segH, TFT_BLACK);
        s_sprite.drawRect(xSeg2, segY, segW, segH, TFT_BLACK);
        s_sprite.drawRect(xSeg3, segY, segW, segH, TFT_BLACK);

        if (current.iaq <= 50.0f) {
            // Good: 仅第 1 段填充黑色
            s_sprite.fillRect(xSeg1 + 2, segY + 2, segW - 4, segH - 4, TFT_BLACK);
        } else if (current.iaq <= 100.0f) {
            // Fair: 前 2 段填充黑色
            s_sprite.fillRect(xSeg1 + 2, segY + 2, segW - 4, segH - 4, TFT_BLACK);
            s_sprite.fillRect(xSeg2 + 2, segY + 2, segW - 4, segH - 4, TFT_BLACK);
        } else {
            // Poor: 3 段全部填充黑色
            s_sprite.fillRect(xSeg1 + 2, segY + 2, segW - 4, segH - 4, TFT_BLACK);
            s_sprite.fillRect(xSeg2 + 2, segY + 2, segW - 4, segH - 4, TFT_BLACK);
            s_sprite.fillRect(xSeg3 + 2, segY + 2, segW - 4, segH - 4, TFT_BLACK);
        }

        s_sprite.drawString(xSeg1 + 7, 185, "OK");
        s_sprite.drawString(xSeg2 + 4, 185, "MED");
        s_sprite.drawString(xSeg3 + 2, 185, "WARN");
    } else {
        s_sprite.drawString(104, 153, "Unit ENV-Pro");
        s_sprite.drawString(104, 172, "Check Port A");
        s_sprite.drawString(104, 185, "SDA:32 SCL:33");
    }

    // ==========================================
    // 5. 推送到电子墨水屏 (高质量全局全刷或局部刷新)
    // ==========================================
    configureEpdWakeMode(fullRefresh);
    s_sprite.pushSprite();
}

// ==========================================
// 6. 空气质量与气体深度实验室界面 (Page 1)
// ==========================================
void renderAirLab(const EnvRecord &current, const char *timeStr, bool fullRefresh) {
    if (!s_spriteCreated) {
        initDashboard();
    }

    // 全屏纯白底色
    s_sprite.fillScreen(TFT_WHITE);
    s_sprite.setTextColor(TFT_BLACK, TFT_WHITE);

    // 1. 顶部状态栏 (drawStatusBar 内部已经在 Y:16 处绘制了单像素底部分割线，此处切勿重复绘制以防线变粗)
    drawStatusBar(timeStr);

    // 分割线：Section 1 (从 Y:16 延伸至 Y:78)
    s_sprite.drawFastVLine(100, 16, 62, TFT_BLACK);
    s_sprite.drawFastHLine(0, 78, 200, TFT_BLACK);

    bool hasValidData = isSensorReady() && (current.iaq > 0.0f || current.temp != 0.0f);

    auto getIaqRating = [](float val) -> const char* {
        if (val <= 50.0f) return "[EXCELLENT]";
        if (val <= 100.0f) return "[GOOD]";
        if (val <= 150.0f) return "[MODERATE]";
        if (val <= 200.0f) return "[POOR]";
        return "HAZARD";
    };

    // 2.1 左半区：Static IAQ (针对密闭雪茄箱，标题与标签靠左，仅大字数值居中)
    s_sprite.setTextSize(1);
    s_sprite.drawString(4, 22, "STATIC IAQ");

    if (hasValidData) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", current.staticIaq);
        s_sprite.setTextSize(2);
        int wVal = s_sprite.textWidth(buf);
        s_sprite.drawString((100 - wVal) / 2, 35, buf);
    } else {
        s_sprite.setTextSize(2);
        int wVal = s_sprite.textWidth("--");
        s_sprite.drawString((100 - wVal) / 2, 35, "--");
    }

    s_sprite.setTextSize(1);
    const char *ratingL = hasValidData ? getIaqRating(current.staticIaq) : "CALIBRATING";
    s_sprite.drawString(6, 59, ratingL);

    // 2.2 右半区：Realtime IAQ (动态空气质量，标题与标签靠左，仅大字数值居中)
    s_sprite.drawString(104, 22, "REALTIME IAQ");

    if (hasValidData) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", current.iaq);
        s_sprite.setTextSize(2);
        int wVal = s_sprite.textWidth(buf);
        s_sprite.drawString(100 + (100 - wVal) / 2, 35, buf);
    } else {
        s_sprite.setTextSize(2);
        int wVal = s_sprite.textWidth("--");
        s_sprite.drawString(100 + (100 - wVal) / 2, 35, "--");
    }

    s_sprite.setTextSize(1);
    const char *ratingR = hasValidData ? getIaqRating(current.iaq) : "CALIBRATING";
    s_sprite.drawString(106, 59, ratingR);

    // 3. 中部气体浓度区：bVOC (ppm) 与 eCO2 (ppm) 双进度条 (Y: 78 ~ 128)
    s_sprite.drawFastVLine(100, 78, 50, TFT_BLACK);
    s_sprite.drawFastHLine(0, 128, 200, TFT_BLACK);

    // 3.1 左侧 bVOC (0 ~ 1.00 ppm 刻度条，内容下移以确保上方分割线完整无缺)
    s_sprite.setTextSize(1);
    if (hasValidData) {
        char vocBuf[24];
        snprintf(vocBuf, sizeof(vocBuf), "bVOC:%.2fppm", current.bvoc);
        s_sprite.drawString(4, 84, vocBuf);

        s_sprite.drawRect(4, 98, 88, 8, TFT_BLACK);
        float vClamped = (current.bvoc > 1.0f) ? 1.0f : ((current.bvoc < 0.0f) ? 0.0f : current.bvoc);
        int vBarW = (int)(vClamped * 84.0f);
        if (vBarW > 0) {
            s_sprite.fillRect(8, 100, vBarW, 4, TFT_BLACK);
        }
        s_sprite.drawString(4, 110, "0.0");
        s_sprite.drawString(66, 110, "1.00");
    } else {
        s_sprite.drawString(4, 84, "bVOC:--ppm");
        s_sprite.drawRect(4, 98, 88, 8, TFT_BLACK);
        s_sprite.drawString(4, 110, "0.0");
        s_sprite.drawString(66, 110, "1.00");
    }

    // 3.2 右侧 eCO2 (400 ~ 2000 ppm 刻度条)
    if (hasValidData) {
        char co2Buf[24];
        snprintf(co2Buf, sizeof(co2Buf), "eCO2:%.0fppm", current.eco2);
        s_sprite.drawString(106, 84, co2Buf);

        s_sprite.drawRect(106, 98, 88, 8, TFT_BLACK);
        float cClamped = (current.eco2 > 2000.0f) ? 2000.0f : ((current.eco2 < 400.0f) ? 400.0f : current.eco2);
        int cBarW = (int)((cClamped - 400.0f) / 1600.0f * 84.0f);
        if (cBarW > 0) {
            s_sprite.fillRect(108, 100, cBarW, 4, TFT_BLACK);
        }
        s_sprite.drawString(106, 110, "400");
        s_sprite.drawString(162, 110, "2000");
    } else {
        s_sprite.drawString(106, 84, "eCO2:--ppm");
        s_sprite.drawRect(106, 98, 88, 8, TFT_BLACK);
        s_sprite.drawString(106, 110, "400");
        s_sprite.drawString(162, 110, "2000");
    }

    // 4. 底部传感器底层物理诊断区 (Y: 128 ~ 200)
    s_sprite.setTextSize(1);
    s_sprite.drawString(6, 133, "-- SENSOR DIAGNOSTICS --");

    char gasBuf[32];
    if (hasValidData && current.gasRes > 0.0f) {
        snprintf(gasBuf, sizeof(gasBuf), "Gas Res : %.1f kOhm", current.gasRes);
    } else {
        snprintf(gasBuf, sizeof(gasBuf), "Gas Res : -- kOhm");
    }
    s_sprite.drawString(6, 147, gasBuf);

    const char *accLabels[] = {"Lv 0 (Learning)", "Lv 1 (Low Acc)", "Lv 2 (Medium)", "Lv 3 (Optimal)"};
    uint8_t a = (current.iaqAccuracy <= 3) ? current.iaqAccuracy : 0;
    char calBuf[32];
    snprintf(calBuf, sizeof(calBuf), "BSEC Acc: %s", accLabels[a]);
    s_sprite.drawString(6, 160, calBuf);

    char stabBuf[32];
    const char *stabStr = (current.stabStatus == 1) ? "STABLE" : "WARMING UP";
    snprintf(stabBuf, sizeof(stabBuf), "Heater  : %s", stabStr);
    s_sprite.drawString(6, 173, stabBuf);

    char runInBuf[32];
    const char *runInStr = (current.runInStatus == 1) ? "COMPLETE" : "IN PROGRESS";
    snprintf(runInBuf, sizeof(runInBuf), "Run-in  : %s", runInStr);
    s_sprite.drawString(6, 186, runInBuf);

    // 5. 推送到屏幕
    configureEpdWakeMode(fullRefresh);
    s_sprite.pushSprite();
}

void renderPowerMenuOverlay(uint8_t selectedIndex) {
    if (!s_spriteCreated) {
        return;
    }

    // 绘制居中悬浮白色对话框 (X: 8, Y: 18, W: 184, H: 164)
    s_sprite.fillRect(8, 18, 184, 164, TFT_WHITE);
    s_sprite.drawRect(8, 18, 184, 164, TFT_BLACK);
    s_sprite.drawRect(10, 20, 180, 160, TFT_BLACK);

    // 标题居中
    s_sprite.setTextSize(1);
    s_sprite.setTextColor(TFT_BLACK, TFT_WHITE);
    const char *title = "SELECT POWER MODE";
    int tw = s_sprite.textWidth(title);
    s_sprite.drawString((180 - tw) / 2, 25, title);
    s_sprite.drawFastHLine(10, 39, 180, TFT_BLACK);

    // 4 项模式列表
    const char *options[4] = {
        "1. Realtime  (3s)",
        "2. Balanced  (1m)",
        "3. Eco ULP   (10m)",
        "4. On-Demand (G38)"
    };

    for (int i = 0; i < 4; ++i) {
        int itemY = 46 + i * 18;
        if (i == selectedIndex) {
            // 反色高亮条
            s_sprite.fillRect(14, itemY - 2, 172, 16, TFT_BLACK);
            s_sprite.setTextColor(TFT_WHITE, TFT_BLACK);
            s_sprite.drawString(20, itemY, options[i]);
            s_sprite.setTextColor(TFT_BLACK, TFT_WHITE);
        } else {
            s_sprite.drawString(20, itemY, options[i]);
        }
    }

    // 分割线 (下方为模式简要说明，绝无按键提示)
    s_sprite.drawFastHLine(10, 120, 180, TFT_BLACK);

    const char *desc[4][3] = {
        {
            "240MHz Active CPU",
            "Continuous 3s sample",
            "Fastest, best for USB"
        },
        {
            "80MHz + Light-Sleep",
            "Instant key response",
            "Balanced daily monitor"
        },
        {
            "10-min Deep Sleep RTC",
            "Periodic data logging",
            "For cigar humidor box"
        },
        {
            "Full Deep Sleep idle",
            "Wake on key, test 1x",
            "Standby for months+"
        }
    };

    uint8_t sel = (selectedIndex < 4) ? selectedIndex : 0;
    s_sprite.drawString(16, 126, desc[sel][0]);
    s_sprite.drawString(16, 140, desc[sel][1]);
    s_sprite.drawString(16, 154, desc[sel][2]);

    M5.M5Ink.setEpdMode(epd_mode_t::epd_fast);
    s_sprite.pushSprite();
}

void renderSplashScreen(const char *statusMsg) {
    if (!s_spriteCreated) {
        initDashboard();
    }

    s_sprite.fillScreen(TFT_WHITE);
    s_sprite.setTextColor(TFT_BLACK, TFT_WHITE);

    // 1. 居中绘制丘吉尔抽雪茄经典剪影 (宽 128, 高 144)
    int imgX = (200 - CHURCHILL_IMG_WIDTH) / 2; // X: 36
    int imgY = 6;
    s_sprite.drawXBitmap(imgX, imgY, CHURCHILL_IMG_BITS,
                         CHURCHILL_IMG_WIDTH, CHURCHILL_IMG_HEIGHT,
                         TFT_BLACK, TFT_WHITE);

    // 2. 经典装饰双分割细线 (左右对称装饰)
    s_sprite.drawFastHLine(14, 156, 36, TFT_BLACK);
    s_sprite.drawFastHLine(150, 156, 36, TFT_BLACK);

    // 3. 经典尊称标牌
    s_sprite.setTextSize(1);
    const char *title = "W. CHURCHILL";
    int tw = s_sprite.textWidth(title);
    s_sprite.drawString((200 - tw) / 2, 152, title);

    // 4. 底部动态状态提示 (如 "Connecting Wi-Fi...")
    if (statusMsg && statusMsg[0]) {
        int sw = s_sprite.textWidth(statusMsg);
        s_sprite.drawString((200 - sw) / 2, 174, statusMsg);
    }

    // 5. 高质量全刷上屏 (消除开机等待过程中的全黑与空白)
    M5.M5Ink.setEpdMode(epd_mode_t::epd_quality);
    s_sprite.pushSprite();
}



