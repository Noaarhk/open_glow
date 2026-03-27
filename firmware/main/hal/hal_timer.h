#ifndef HAL_TIMER_H
#define HAL_TIMER_H

/*
 * 타이머 및 Watchdog 하드웨어 추상화 레이어
 *
 * 시간 조회:
 *   ESP-IDF의 esp_timer(마이크로초 정밀도)를 밀리초로 변환하여 제공.
 *   현재 event_queue.c, button_handler.c, device_fsm.c에서
 *   각각 get_time_ms()를 중복 정의하고 있는데, 향후 이 HAL로 통일 가능.
 *
 * Watchdog:
 *   메인 루프가 설정 시간(5초) 내에 feed()를 호출하지 않으면
 *   ESP32가 자동으로 리셋됨. 무한 루프 버그나 행(hang) 방지.
 *
 * 사용 예:
 *   uint32_t now = hal_timer_get_ms();
 *   hal_watchdog_feed();  // 메인 루프 시작마다 호출
 */

#include <stdint.h>

/* 타이머 초기화 */
void hal_timer_init(void);

/* 현재 시각 (밀리초 단위, 부팅 후 경과 시간) */
uint32_t hal_timer_get_ms(void);

/* Task Watchdog 타이머 초기화 (timeout_ms: 타임아웃 밀리초) */
void hal_watchdog_init(uint32_t timeout_ms);

/* Watchdog 타이머 리셋 (메인 루프 매 사이클에서 호출) */
void hal_watchdog_feed(void);

#endif /* HAL_TIMER_H */
