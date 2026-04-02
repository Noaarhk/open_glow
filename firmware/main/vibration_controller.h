#ifndef VIBRATION_CONTROLLER_H
#define VIBRATION_CONTROLLER_H

/*
 * 진동 모터 컨트롤러 — Coin Vibration Motor PWM 제어
 *
 * 햅틱 피드백(촉각 반응) 제공.
 * 버튼 누를 때 "딸깍" 느낌, 에러 시 경고 진동 등.
 *
 * PWM 주파수 20kHz (가청 범위 밖 → 모터 소음 없음).
 * 13bit 해상도로 세밀한 세기 조절.
 *
 * pulse() 동작 원리:
 *   "200ms ON → 100ms OFF → 200ms ON → 100ms OFF → 200ms ON"
 *   이런 패턴을 vibration_pulse(200, 100, 3)으로 한 번에 설정.
 *   실제 on/off 전환은 vibration_update()에서 시간 체크하며 자동 수행.
 *   (Django의 Celery beat가 주기적으로 태스크를 실행하는 것과 유사)
 */

#include <stdint.h>

/* 초기화 (PWM 채널 설정) */
void vibration_init(void);

/* void vibration_set_intensity(uint8_t level); — 미사용, Phase 5 연속 진동 모드 시 활성화 예정 */

/*
 * 펄스 진동 시작
 * on_ms: 진동 ON 시간
 * off_ms: 진동 OFF 시간
 * count: 반복 횟수
 * 예: vibration_pulse(200, 100, 3) → 200ms 진동, 100ms 쉼, 3번 반복
 */
void vibration_pulse(uint16_t on_ms, uint16_t off_ms, uint8_t count);

/* void vibration_start(void); — 미사용, Phase 5 연속 진동 모드 시 활성화 예정 */

/* 진동 중지 */
void vibration_stop(void);

/* 메인 루프에서 호출 (펄스 패턴 시간 관리) */
void vibration_update(void);

#endif /* VIBRATION_CONTROLLER_H */
