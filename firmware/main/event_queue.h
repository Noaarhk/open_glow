#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

/*
 * OpenGlow 이벤트 큐
 *
 * 링 버퍼 기반 FIFO 큐. 모듈 간 비동기 이벤트 전달.
 * - 버튼, 센서, BLE 콜백 등이 push()로 이벤트를 넣음
 * - FSM이 pop()으로 이벤트를 꺼내서 처리
 * - push()는 ISR-safe (critical section 사용)
 * - 안전 이벤트는 오버플로 시에도 드롭하지 않음
 */

#include <stdbool.h>
#include <stdint.h>

/* 이벤트 타입 정의 */
typedef enum {
    /* 버튼 이벤트 (물리 버튼 2개: 전원 + 모드) */
    EVENT_BTN_POWER_SHORT,          /* 전원 버튼 짧게 (<2초) */
    EVENT_BTN_POWER_LONG,           /* 전원 버튼 길게 (≥2초) */
    EVENT_BTN_POWER_VLONG,          /* 전원 버튼 매우 길게 (≥3초, 강제 종료) */
    EVENT_BTN_MODE_SHORT,           /* 모드 버튼 짧게 (모드 순환 / 세기 증가) */
    EVENT_BTN_MODE_LONG,            /* 모드 버튼 길게 (세기 감소) */

    /* 안전 이벤트 */
    EVENT_SAFETY_TEMP_WARNING,      /* 온도 경고 (40~50도) */
    EVENT_SAFETY_TEMP_CRITICAL,     /* 온도 위험 (50도↑) → 비상 차단 */
    EVENT_SAFETY_BATTERY_LOW,       /* 배터리 부족 (10%) */
    EVENT_SAFETY_BATTERY_CRITICAL,  /* 배터리 위험 (5%) → 강제 종료 */
    EVENT_SAFETY_AUTO_WARNING,      /* 8분 경과 경고 */
    EVENT_SAFETY_AUTO_TIMEOUT,      /* 10분 자동 종료 */

    /* 피부 접촉 이벤트 */
    EVENT_SKIN_CONTACT_ON,          /* 피부 접촉 감지 */
    EVENT_SKIN_CONTACT_OFF,         /* 피부 접촉 해제 */

    /* 충전 이벤트 */
    EVENT_CHARGE_CONNECTED,         /* 충전기 연결 */
    EVENT_CHARGE_DISCONNECTED,      /* 충전기 분리 */
    EVENT_CHARGE_COMPLETE,          /* 충전 완료 */

    /* BLE 이벤트 */
    EVENT_BLE_CONNECTED,            /* BLE 연결됨 */
    EVENT_BLE_DISCONNECTED,         /* BLE 연결 끊김 */
    EVENT_BLE_MODE_CHANGE,          /* 앱에서 모드 변경 요청 */
    EVENT_BLE_INTENSITY_CHANGE,     /* 앱에서 세기 변경 요청 */
    EVENT_BLE_LED_COLOR_CHANGE,     /* 앱에서 LED 색상 변경 요청 */

    EVENT_COUNT
} event_type_t;

/* 이벤트 구조체 */
typedef struct {
    event_type_t type;
    uint32_t data;                  /* 부가 데이터 (모드 값, 세기 값 등) */
    uint32_t timestamp_ms;          /* 이벤트 발생 시각 */
} event_t;

/* 초기화 */
void event_queue_init(void);

/* 이벤트 추가 (ISR-safe, BLE 콜백에서도 호출 가능) */
bool event_queue_push(event_t event);

/* 이벤트 꺼내기 (메인 루프의 fsm_update에서 호출) */
bool event_queue_pop(event_t *event);

/* 큐 상태 조회 */
bool event_queue_is_empty(void);
uint8_t event_queue_count(void);

#endif /* EVENT_QUEUE_H */
