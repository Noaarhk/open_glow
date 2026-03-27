#ifndef HAL_ADC_H
#define HAL_ADC_H

/*
 * ADC 하드웨어 추상화 레이어
 *
 * ESP32의 12비트 ADC를 래핑.
 * battery_monitor에서 배터리 전압(ADC1_CH6)과 온도(ADC1_CH7)를 읽을 때 사용.
 *
 * ESP32 ADC1 채널 매핑:
 *   ADC1_CHANNEL_4 → GPIO32 (피부 접촉 — 현재 미사용, TTP223 GPIO 방식)
 *   ADC1_CHANNEL_6 → GPIO34 (배터리 전압)
 *   ADC1_CHANNEL_7 → GPIO35 (NTC 온도 센서)
 *
 * 사용 예:
 *   uint16_t raw = hal_adc_read(ADC1_CHANNEL_6);  // 0~4095
 */

#include "driver/adc.h"
#include <stdint.h>

/* ADC 서브시스템 초기화 (12bit 해상도, 감쇠 설정) */
void hal_adc_init(void);

/* 지정 채널에서 ADC 값 읽기 (0~4095, 12bit) */
uint16_t hal_adc_read(adc1_channel_t channel);

#endif /* HAL_ADC_H */
