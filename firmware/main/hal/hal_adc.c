/*
 * ADC HAL 구현 (ESP32)
 *
 * ESP-IDF의 ADC1 드라이버를 래핑.
 * ADC1만 사용 (ADC2는 Wi-Fi와 충돌하므로 사용 불가).
 *
 * 감쇠(attenuation) 설정:
 *   ADC_ATTEN_DB_12 → 입력 전압 범위 0~3.3V (ESP32 최대)
 *   배터리 전압은 분압 회로를 거쳐 3.3V 이하로 들어온다고 가정.
 */

#include "hal_adc.h"
#include "../debug_log.h"

static const char *TAG = "HAL_ADC";

void hal_adc_init(void)
{
    /* ADC1 해상도: 12bit (0~4095) */
    adc1_config_width(ADC_WIDTH_BIT_12);

    /* 배터리 전압 채널 (GPIO34) */
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12);

    /* 온도 센서 채널 (GPIO35) */
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_12);

    LOG_INFO("ADC HAL initialized (12bit, atten=12dB)");
}

uint16_t hal_adc_read(adc1_channel_t channel)
{
    int raw = adc1_get_raw(channel);
    if (raw < 0) {
        LOG_ERROR("ADC read failed on channel %d", channel);
        return 0;
    }
    return (uint16_t)raw;
}
