#include "sensor_bsec.h"
#include <Wire.h>
#include <bsec.h>

static Bsec iaqSensor;
static EnvRecord s_latestRecord;
static bool s_sensorReady = false;
static uint32_t s_lastRetryMs = 0;
static uint8_t s_sensorI2cAddr = 0x77;

// 高精度 I2C 读回调：支持 Repeated Start
static int8_t userI2cRead(uint8_t regAddr, uint8_t *regData, uint32_t length, void *intfPtr) {
    uint8_t devAddr = (uint8_t)(intptr_t)intfPtr;
    Wire.beginTransmission(devAddr);
    Wire.write(regAddr);
    if (Wire.endTransmission(false) != 0) {
        Wire.beginTransmission(devAddr);
        Wire.write(regAddr);
        if (Wire.endTransmission(true) != 0) {
            return -1;
        }
    }
    uint8_t bytesRead = Wire.requestFrom((int)devAddr, (int)length);
    if (bytesRead != length) {
        return -1;
    }
    for (uint32_t i = 0; i < length; i++) {
        regData[i] = Wire.read();
    }
    return 0;
}

// 高精度 I2C 写回调
static int8_t userI2cWrite(uint8_t regAddr, const uint8_t *regData, uint32_t length, void *intfPtr) {
    uint8_t devAddr = (uint8_t)(intptr_t)intfPtr;
    Wire.beginTransmission(devAddr);
    Wire.write(regAddr);
    for (uint32_t i = 0; i < length; i++) {
        Wire.write(regData[i]);
    }
    return (Wire.endTransmission() == 0) ? 0 : -1;
}

// 微秒真实延时
static void userDelayUs(uint32_t period, void *intfPtr) {
    (void)intfPtr;
    if (period >= 1000) {
        delay(period / 1000);
        delayMicroseconds(period % 1000);
    } else {
        delayMicroseconds(period);
    }
}

// I2C 总线扫描
static void scanI2CBus() {
    Serial.println("[I2C Scan] Scanning Grove Port A (SDA: GPIO 32, SCL: GPIO 33)...");
    uint8_t foundCount = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  -> [I2C Device Detected] Address: 0x%02X\n", addr);
            foundCount++;
        }
    }
    if (foundCount == 0) {
        Serial.println("  -> [I2C WARNING] No devices found on Grove Port A!");
    } else {
        Serial.printf("  -> [I2C Scan Complete] Found %u device(s).\n", foundCount);
    }
}

// 读取 Chip ID (0xD0)
static uint8_t readBme688ChipId(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(0xD0);
    if (Wire.endTransmission(false) == 0 || Wire.endTransmission(true) == 0) {
        if (Wire.requestFrom((int)addr, 1) == 1) {
            return Wire.read();
        }
    }
    return 0xFF;
}

static uint8_t detectSensorAddress() {
    scanI2CBus();

    uint8_t id77 = readBme688ChipId(0x77);
    if (id77 == 0x61) {
        Serial.println("[BME688] Confirmed BME688 at 0x77 (Chip ID: 0x61).");
        return 0x77;
    }

    uint8_t id76 = readBme688ChipId(0x76);
    if (id76 == 0x61) {
        Serial.println("[BME688] Confirmed BME688 at 0x76 (Chip ID: 0x61).");
        return 0x76;
    }

    Serial.println("[BME688] Warning: Chip ID 0x61 not found, defaulting to 0x77.");
    return 0x77;
}

bool initBSEC() {
    Serial.println("========================================");
    Serial.println("[BSEC] Initializing Bosch BSEC for Unit ENV-Pro (BME688)...");

    Wire.begin(32, 33, 100000UL);
    delay(50);

    s_sensorI2cAddr = detectSensorAddress();

    Bsec::wireObj = &Wire;
    iaqSensor.begin(BME68X_I2C_INTF, userI2cRead, userI2cWrite, userDelayUs, (void*)(intptr_t)s_sensorI2cAddr);
    Wire.begin(32, 33, 100000UL);

    if (iaqSensor.bme68xStatus != BME68X_OK && iaqSensor.bme68xStatus != BME68X_WARNING) {
        Serial.printf("[BSEC] ERROR: BME68x sensor init failed, status = %d\n", iaqSensor.bme68xStatus);
        s_sensorReady = false;
        return false;
    }

    if (iaqSensor.bsecStatus < BSEC_OK) {
        Serial.printf("[BSEC] ERROR: BSEC library init failed, status = %d\n", iaqSensor.bsecStatus);
        s_sensorReady = false;
        return false;
    }

    // 订阅全面的 BSEC 虚拟传感器输出 (包含 Static IAQ, bVOC 与原始 Gas 阻抗)
    bsec_virtual_sensor_t sensorList[] = {
        BSEC_OUTPUT_RAW_TEMPERATURE,
        BSEC_OUTPUT_RAW_PRESSURE,
        BSEC_OUTPUT_RAW_HUMIDITY,
        BSEC_OUTPUT_RAW_GAS,
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
        BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
        BSEC_OUTPUT_STABILIZATION_STATUS,
        BSEC_OUTPUT_RUN_IN_STATUS
    };

    iaqSensor.updateSubscription(sensorList, sizeof(sensorList) / sizeof(sensorList[0]), BSEC_SAMPLE_RATE_LP);

    if (iaqSensor.bsecStatus < BSEC_OK) {
        Serial.printf("[BSEC] ERROR: updateSubscription failed, status = %d\n", iaqSensor.bsecStatus);
        s_sensorReady = false;
        return false;
    }

    s_sensorReady = true;
    loadBsecState();

    Serial.println("[BSEC] Initialization successful!");
    Serial.println("========================================");
    return true;
}

RTC_DATA_ATTR static uint8_t s_rtcBsecState[BSEC_MAX_STATE_BLOB_SIZE] = {0};
RTC_DATA_ATTR static bool s_rtcBsecValid = false;

void saveBsecState() {
    if (s_sensorReady) {
        iaqSensor.getState(s_rtcBsecState);
        s_rtcBsecValid = true;
        Serial.println("[BSEC] Algorithm state saved to RTC memory.");
    }
}

void loadBsecState() {
    if (s_sensorReady && s_rtcBsecValid) {
        iaqSensor.setState(s_rtcBsecState);
        Serial.println("[BSEC] Algorithm state restored from RTC memory.");
    }
}

void setBsecSampleRate(float sampleRate) {
    if (!s_sensorReady) return;
    bsec_virtual_sensor_t sensorList[] = {
        BSEC_OUTPUT_RAW_TEMPERATURE,
        BSEC_OUTPUT_RAW_PRESSURE,
        BSEC_OUTPUT_RAW_HUMIDITY,
        BSEC_OUTPUT_RAW_GAS,
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
        BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
        BSEC_OUTPUT_STABILIZATION_STATUS,
        BSEC_OUTPUT_RUN_IN_STATUS
    };
    iaqSensor.updateSubscription(sensorList, sizeof(sensorList) / sizeof(sensorList[0]), sampleRate);
    Serial.printf("[BSEC] Updated sample rate to: %.4f Hz\n", sampleRate);
}

bool runBSEC(EnvRecord &outRecord) {
    if (!s_sensorReady) {
        return false;
    }

    if (iaqSensor.run()) {
        float temp = iaqSensor.rawTemperature;
        float humi = iaqSensor.rawHumidity;
        float press = iaqSensor.pressure / 100.0f; // Pa 转 hPa

        float iaq = iaqSensor.iaq;
        float staticIaq = (iaqSensor.staticIaq > 0.0f) ? iaqSensor.staticIaq : iaq;
        float eco2 = (iaqSensor.co2Equivalent > 0.0f) ? iaqSensor.co2Equivalent : (400.0f + iaq * 10.0f);
        float bvoc = iaqSensor.breathVocEquivalent;
        float gasRes = iaqSensor.gasResistance / 1000.0f; // 转化为 kΩ
        uint8_t acc = iaqSensor.iaqAccuracy;
        uint8_t stab = (uint8_t)iaqSensor.stabStatus;
        uint8_t runIn = (uint8_t)iaqSensor.runInStatus;

        s_latestRecord.timestamp = (uint32_t)(iaqSensor.outputTimestamp / 1000);
        s_latestRecord.temp = temp;
        s_latestRecord.humi = humi;
        s_latestRecord.press = press;
        s_latestRecord.iaq = iaq;
        s_latestRecord.staticIaq = staticIaq;
        s_latestRecord.eco2 = eco2;
        s_latestRecord.bvoc = bvoc;
        s_latestRecord.gasRes = gasRes;
        s_latestRecord.iaqAccuracy = acc;
        s_latestRecord.stabStatus = stab;
        s_latestRecord.runInStatus = runIn;

        outRecord = s_latestRecord;
        return true;
    }

    // 诊断：每 5 秒打印一次底层状态码
    static uint32_t s_lastDiagMs = 0;
    if (millis() - s_lastDiagMs >= 5000) {
        s_lastDiagMs = millis();
        Serial.printf("[BSEC Diag] bsecStatus=%d bme68xStatus=%d\n",
                      (int)iaqSensor.bsecStatus, (int)iaqSensor.bme68xStatus);
    }
    return false;
}

bool waitForFirstReading(EnvRecord &outRec, uint32_t timeoutMs) {
    if (!s_sensorReady) return false;

    // 先检查是否已有有效数据
    if (s_latestRecord.temp != 0.0f || s_latestRecord.humi != 0.0f) {
        outRec = s_latestRecord;
        return true;
    }

    Serial.printf("[BSEC] Waiting for first conversion (up to %u ms)...\n", timeoutMs);
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (runBSEC(outRec)) {
            Serial.printf("[BSEC] First reading ready! T:%.1fC, H:%.1f%%, P:%.0fhPa, IAQ:%.0f\n",
                          outRec.temp, outRec.humi, outRec.press, outRec.iaq);
            return true;
        }
        delay(100);
    }

    Serial.println("[BSEC] First reading wait timeout, will update in background.");
    return false;
}

EnvRecord getLatestEnvRecord() {
    return s_latestRecord;
}

bool isSensorReady() {
    return s_sensorReady && (s_latestRecord.temp != 0.0f || s_latestRecord.humi != 0.0f);
}

bool checkAndRecoverSensor() {
    // 已初始化成功：只需等待 BSEC 产出数据，绝不能重复初始化(会重置算法)
    if (s_sensorReady) {
        return true;
    }

    if (millis() - s_lastRetryMs >= 5000) {
        s_lastRetryMs = millis();
        Serial.println("[Sensor Recovery] Init failed earlier, retrying...");
        return initBSEC();
    }
    return false;
}
