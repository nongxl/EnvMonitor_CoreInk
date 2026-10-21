#include "battery.h"
#include <esp_adc_cal.h>

#define BATTERY_ADC_PIN 35

float getBatteryVoltage() {
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    esp_adc_cal_characteristics_t *adc_chars =
        (esp_adc_cal_characteristics_t *)calloc(1, sizeof(esp_adc_cal_characteristics_t));
    if (!adc_chars) {
        return 3.7f;
    }

    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 3600, adc_chars);
    uint16_t adcVal = analogRead(BATTERY_ADC_PIN);
    uint32_t batVolMv = esp_adc_cal_raw_to_voltage(adcVal, adc_chars);
    free(adc_chars);

    // CoreInk 板载分压比 25.1 / 5.1
    float batVol = (float)batVolMv * 25.1f / 5.1f / 1000.0f;
    return batVol;
}

int getBatteryPercentage() {
    float v = getBatteryVoltage();
    if (v >= 4.15f) return 100;
    if (v <= 3.35f) return 0;
    // 简易线性拟合 3.35V~4.15V
    int pct = (int)((v - 3.35f) / (4.15f - 3.35f) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
