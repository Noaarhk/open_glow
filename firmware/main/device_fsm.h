#ifndef DEVICE_FSM_H
#define DEVICE_FSM_H

/*
 * OpenGlow 상태 머신 (FSM)
 *
 * 디바이스의 전체 동작 흐름을 제어하는 중앙 컨트롤러.
 * event_queue에서 이벤트를 꺼내서 상태 전이를 수행하고,
 * 상태에 따라 출력 모듈(EMS, LED, 진동)을 제어.
 *
 * 상태 전이:
 *   OFF → IDLE → MODE_SELECT → RUNNING → PAUSED
 *                                       → CHARGING
 *                                       → ERROR → SHUTDOWN → OFF
 */

#include "openglow_config.h"

/* FSM 컨텍스트 (현재 상태 + 관련 데이터) */
typedef struct {
    device_state_t current_state;
    device_state_t previous_state;      /* 디버그/복구용 */
    device_mode_t current_mode;         /* PULSE, MICRO, EMS, THERMAL */
    uint8_t intensity_level;            /* 1~5 */
    uint32_t running_start_time_ms;     /* 세션 시작 시각 */
    uint32_t total_session_time_ms;     /* 누적 사용 시간 */
    uint32_t state_enter_time_ms;       /* 현재 상태 진입 시각 */
    uint32_t last_interaction_ms;       /* 마지막 사용자 입력 시각 */
    bool skin_contact;                  /* 피부 접촉 여부 */
    bool is_charging;                   /* 충전 중 여부 */
    uint8_t error_code;                 /* 현재 에러 코드 */
} device_context_t;

/* 초기화 (STATE_OFF에서 시작) */
void fsm_init(void);

/* 메인 루프에서 호출: 이벤트 소비 + 상태 전이 + 타임아웃 체크 */
void fsm_update(void);

/* 현재 상태 조회 */
device_state_t fsm_get_state(void);

/* 현재 컨텍스트 조회 (BLE 전송용) */
const device_context_t* fsm_get_context(void);

/* 상태 이름 문자열 (로그용) */
const char* fsm_state_name(device_state_t state);

#endif /* DEVICE_FSM_H */
