#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

/*
 * OpenGlow 버튼 핸들러
 *
 * 물리 버튼 2개(전원, 모드)의 디바운싱 + 길게/짧게 누름 판별.
 * 판별된 이벤트는 event_queue에 자동 push.
 *
 * 동작:
 * - 50ms 디바운스 (10ms 주기 × 5회 연속 동일)
 * - 짧게 누름: <2초 (버튼 해제 시 발생)
 * - 길게 누름: ≥2초 (누르고 있는 중 즉시 발생, 해제 안 기다림)
 * - 매우 길게: ≥3초 (누르고 있는 중 즉시 발생)
 */

/* GPIO 초기화 (풀업 설정 포함) */
void button_init(void);

/* 메인 루프에서 10ms 주기로 호출 */
void button_update(void);

#endif /* BUTTON_HANDLER_H */
