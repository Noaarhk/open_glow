#ifndef SAFETY_MANAGER_H
#define SAFETY_MANAGER_H

/*
 * 안전 관리자 모듈 — 이중 안전 구조
 *
 * Level 1 (정상 경로):
 *   safety_update() → 위험 감지 → EVENT_SAFETY_* push → FSM이 처리
 *
 * Level 2 (비상 경로, FSM 우회):
 *   safety_update() → CRITICAL 조건 → safety_emergency_shutdown() 직접 호출
 *   → EMS_ENABLE 핀 LOW (하드웨어 차단) + ems_emergency_stop()
 *   → 이후 FSM에도 이벤트 push (로깅/상태 동기화)
 *
 * 원칙: FSM에 버그가 있더라도, Level 2가 하드웨어 레벨에서 출력을 차단.
 *
 * 보호 기능:
 *   1. 과열 보호: 40°C 경고, 45°C 출력 제한, 50°C 비상 차단
 *   2. 저전압 보호: 10% 출력 제한, 5% 비상 차단
 *   3. 자동 꺼짐: 8분 경고, 10분 자동 종료
 *   4. 샷 쿨다운: 연속 출력 간 최소 500ms 간격
 */

#include <stdbool.h>
#include <stdint.h>

/* 안전 상태 열거형 */
typedef enum {
    SAFETY_OK,
    SAFETY_TEMP_WARNING,
    SAFETY_TEMP_CRITICAL,
    SAFETY_BATTERY_LOW,
    SAFETY_BATTERY_CRITICAL,
    SAFETY_EMERGENCY_SHUTDOWN
} safety_status_t;

/* 초기화 */
void safety_init(void);

/* 메인 루프에서 호출 (battery_update 이후에 호출해야 최신 데이터 사용) */
void safety_update(void);

/* 현재 안전 상태 */
safety_status_t safety_get_status(void);

/* 샷 쿨다운 체크 */
bool safety_can_fire_shot(void);

/* 샷 시각 기록 */
void safety_record_shot(void);

/* 안전 출력 제한값 (0.0~1.0, EMS에 전달) */
float safety_get_output_limit(void);

/* 비상 차단 (Level 2: FSM 우회, 하드웨어 직접 차단) */
void safety_emergency_shutdown(void);

#endif /* SAFETY_MANAGER_H */
