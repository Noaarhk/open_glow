#ifndef EMS_CONTROLLER_H
#define EMS_CONTROLLER_H

/*
 * EMS 컨트롤러 — 모드별 PWM 펄스 생성
 *
 * 4가지 뷰티 모드에 맞는 주파수/듀티비로 전기 자극 출력을 제어.
 * Django로 비유하면, 여러 종류의 이메일 템플릿(모드)에 따라
 * 발송 빈도(주파수)와 발송량(듀티비)을 조절하는 서비스와 같음.
 *
 * 모드별 동작:
 *   PULSE    — 1kHz  LEDC PWM, 10~50% 듀티
 *   MICRO    — 10Hz  소프트웨어 GPIO 토글 (LEDC 최소 주파수 제한)
 *   EMS      — 3kHz  LEDC PWM, 15~60% 듀티
 *   THERMAL  — 5kHz  LEDC PWM, 10~45% 듀티
 *
 * 안전 구조:
 *   PIN_EMS_ENABLE (GPIO26)을 별도로 두어,
 *   소프트웨어 버그로 PWM이 멈추지 않아도 하드웨어 레벨에서 차단 가능.
 *   → ems_emergency_stop()이 이 핀을 LOW로 내림.
 */

#include "openglow_config.h"
#include <stdbool.h>

/* 초기화 (PWM 채널 설정 + EMS_ENABLE 핀 설정) */
void ems_init(void);

/* 모드 설정 (주파수 변경, 출력 중이면 먼저 stop 필요) */
void ems_set_mode(device_mode_t mode);

/* 세기 설정 (1~5, 모드별 듀티비 범위 내에서 매핑) */
void ems_set_intensity(uint8_t level);

/* 안전 출력 제한 (0.0~1.0, safety_manager가 설정) */
void ems_set_output_limit(float limit);

/* 출력 시작 (EMS_ENABLE HIGH + PWM 시작) */
void ems_start(void);

/* 출력 중지 (PWM 듀티 0, 정상 종료) */
void ems_stop(void);

/* 비상 차단 (EMS_ENABLE LOW + PWM 중지, FSM 우회 가능) */
void ems_emergency_stop(void);

/* 메인 루프에서 호출 (MICRO 모드 소프트웨어 토글, Phase 5: PID 업데이트) */
void ems_update(void);

/* 현재 듀티비 조회 (0.0~1.0, BLE 전송용) */
float ems_get_current_duty(void);

/* 출력 중 여부 */
bool ems_is_active(void);

#endif /* EMS_CONTROLLER_H */
