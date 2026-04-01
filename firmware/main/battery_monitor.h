#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

/*
 * 배터리/온도 모니터 모듈
 *
 * ADC로 배터리 전압(GPIO34)과 NTC 온도 센서(GPIO35)를 측정.
 * 충전 IC의 STAT 핀(GPIO27)으로 충전 상태 감지.
 *
 * 노이즈 제거:
 *   ADC 값에 이동평균 필터(16샘플)를 적용하여 안정적인 읽기 제공.
 *   safety_manager가 이 필터링된 값을 기반으로 판단.
 *
 * 호출 주기:
 *   - battery_update()는 매 루프(10ms)마다 호출되지만,
 *     ADC 읽기는 내부적으로 5초 간격으로만 수행 (전력 절약)
 *   - 충전 상태 GPIO는 매 루프마다 체크 (즉시 반응 필요)
 */

#include <stdbool.h>
#include <stdint.h>

/* 초기화 (GPIO 설정 + 필터 초기화) */
void battery_init(void);

/* 메인 루프에서 호출 (ADC 5초 간격 + 충전 GPIO 매 루프) */
void battery_update(void);

/* 배터리 잔량 (0~100%) */
uint8_t battery_get_percent(void);

/* 배터리 전압 (V) */
float battery_get_voltage(void);

/* 온도 (°C) */
float battery_get_temperature(void);

/* 충전 중 여부 */
bool battery_is_charging(void);

/* ADC 읽기 유효성 (센서 고장 감지) */
bool battery_is_valid(void);

#endif /* BATTERY_MONITOR_H */
