#ifndef HAL_PWM_H
#define HAL_PWM_H

/*
 * PWM 하드웨어 추상화 레이어
 *
 * ESP32의 LEDC(LED Control) 드라이버를 래핑하여 PWM 출력을 제어.
 * ems_controller와 vibration_controller에서 사용.
 *
 * ESP32 LEDC 구조:
 *   - 타이머 4개 (0~3) × 채널 8개 (0~7)
 *   - 각 채널은 하나의 타이머에 연결
 *   - 타이머가 주파수를 결정, 채널이 듀티비를 결정
 *
 * 채널 할당:
 *   PWM_CH_EMS       → LEDC 채널 0 + 타이머 0 → GPIO25
 *   PWM_CH_VIBRATION → LEDC 채널 1 + 타이머 1 → GPIO19
 *
 * 사용 예:
 *   hal_pwm_configure(PWM_CH_EMS, PIN_EMS_PWM, 3000, 13);  // 3kHz, 13bit
 *   hal_pwm_set_duty(PWM_CH_EMS, 4096);  // 50% (8192 * 0.5)
 *   hal_pwm_start(PWM_CH_EMS);
 */

#include "driver/gpio.h"
#include <stdint.h>

/* PWM 채널 정의 */
typedef enum {
    PWM_CH_EMS = 0,         /* EMS/BOOSTER/AIRSHOT 출력 */
    PWM_CH_VIBRATION,       /* 진동 모터 */
    PWM_CH_COUNT
} hal_pwm_channel_t;

/* PWM 서브시스템 초기화 */
void hal_pwm_init(void);

/*
 * 채널별 PWM 설정
 * ch: 채널
 * pin: GPIO 출력 핀
 * freq_hz: 주파수 (Hz)
 * resolution_bits: 해상도 (비트, 예: 13 → 0~8191)
 */
void hal_pwm_configure(hal_pwm_channel_t ch, gpio_num_t pin,
                       uint32_t freq_hz, uint8_t resolution_bits);

/* 듀티비 설정 (0 ~ 2^resolution_bits - 1) */
void hal_pwm_set_duty(hal_pwm_channel_t ch, uint32_t duty);

/* 주파수 변경 (모드 전환 시 사용) */
void hal_pwm_set_frequency(hal_pwm_channel_t ch, uint32_t freq_hz);

/* PWM 출력 시작 */
void hal_pwm_start(hal_pwm_channel_t ch);

/* PWM 출력 중지 (듀티비 0 + 업데이트) */
void hal_pwm_stop(hal_pwm_channel_t ch);

#endif /* HAL_PWM_H */
