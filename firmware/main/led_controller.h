#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

/*
 * LED 컨트롤러 — WS2812B NeoPixel RGB LED 제어
 *
 * ESP32의 RMT(Remote Control Transceiver) 드라이버를 사용.
 * WS2812B는 특수한 타이밍(400ns/800ns)으로 데이터를 전송해야 하는데,
 * 소프트웨어로는 이 타이밍을 정확히 맞추기 어려워 하드웨어(RMT)를 사용.
 *
 * 동작 모드:
 *   SOLID   — 단색 고정 점등 (RUNNING 상태)
 *   BLINK   — 깜빡임 (MODE_SELECT, PAUSED, ERROR)
 *   BREATHE — 밝기가 서서히 오르내림 (IDLE)
 *   FADE_OUT — 서서히 꺼짐 (SHUTDOWN)
 *   OFF     — 소등
 *
 * 모드별 기본 색상:
 *   PULSE    → 주황 (255, 100, 0)
 *   MICRO    → 하늘 (0, 200, 255)
 *   EMS      → 초록 (0, 255, 100)
 *   THERMAL  → 보라 (200, 0, 255)
 */

#include "openglow_config.h"
#include <stdint.h>

/* 초기화 (RMT 드라이버 + led_strip 설정) */
void led_init(void);

/* 단색 설정 (RGB 직접 지정) */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/* 모드별 기본색 적용 */
void led_set_mode_color(device_mode_t mode);

/* 밝기 설정 (0~255, RGB 값에 스케일링 적용) */
void led_set_brightness(uint8_t brightness);

/* 깜빡임 모드 시작 (on_ms/off_ms 주기) */
void led_blink(uint16_t on_ms, uint16_t off_ms);

/* 숨쉬기 모드 시작 (period_ms 주기로 밝기 오르내림) */
void led_breathe(uint16_t period_ms);

/* 페이드아웃 시작 (duration_ms 동안 서서히 꺼짐) */
void led_fade_out(uint16_t duration_ms);

/* 배터리 잔량 표시 (빨강→주황→초록 그라데이션) */
void led_show_battery_level(uint8_t percent);

/* 메인 루프에서 호출 (패턴 업데이트: blink 토글, breathe 밝기 변경 등) */
void led_update(void);

/* LED 소등 */
void led_off(void);

#endif /* LED_CONTROLLER_H */
