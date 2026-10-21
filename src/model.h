#pragma once
#include <stdint.h>

/**
 * @brief 环境监测单次快照与历史持久化记录结构
 */
struct EnvRecord {
    uint32_t timestamp;  // 时间戳 (Unix epoch 或自启动小时数)
    float temp;          // 温度 (°C)
    float humi;          // 相对湿度 (%RH)
    float press;         // 大气压强 (hPa)
    float iaq;           // 综合动态空气质量指数 (0~500)
    float staticIaq;     // 静态空气质量指数 (0~500，专为密闭雪茄箱/静止空间优化)
    float eco2;          // 等效 CO2 浓度 (ppm)
    float bvoc;          // 呼吸/挥发性有机物当量 (ppm)
    float gasRes;        // 气体传感器原始物理阻抗 (kOhm)
    uint8_t iaqAccuracy; // 算法校准精度等级 (0=未校准, 1=校准中, 2=良好, 3=高精)
    uint8_t stabStatus;  // 传感器加热稳定状态 (0=稳定中, 1=已稳定)
    uint8_t runInStatus; // 传感器预热老化状态 (0=预热中, 1=已完成)

    EnvRecord() 
        : timestamp(0), temp(0.0f), humi(0.0f), press(0.0f),
          iaq(0.0f), staticIaq(0.0f), eco2(0.0f), bvoc(0.0f), gasRes(0.0f),
          iaqAccuracy(0), stabStatus(0), runInStatus(0) {}

    EnvRecord(uint32_t ts, float t, float h, float p, float q, float c, uint8_t acc = 0)
        : timestamp(ts), temp(t), humi(h), press(p),
          iaq(q), staticIaq(q), eco2(c), bvoc(0.0f), gasRes(0.0f),
          iaqAccuracy(acc), stabStatus(1), runInStatus(1) {}
};
