#ifndef SKIN_CONTACT_H
#define SKIN_CONTACT_H

/*
 * 피부 접촉 감지 모듈
 *
 * TTP223 정전식 터치 센서 사용.
 * TTP223이 내부적으로 정전용량 변화를 감지하고 디바운싱까지 처리.
 * ESP32는 결과를 GPIO 디지털 입력으로 읽기만 하면 됨.
 *   HIGH = 접촉 감지, LOW = 비접촉
 *
 * 추가 소프트웨어 디바운스(100ms)로 상태 전환 시 떨림(chatter) 방지.
 * 접촉/해제 시 EVENT_SKIN_CONTACT_ON/OFF를 event_queue에 push.
 */

#include <stdbool.h>

/* 초기화 (GPIO 입력 설정) */
void skin_contact_init(void);

/* 메인 루프에서 호출 (접촉 상태 체크 + 이벤트 push) */
void skin_contact_update(void);

/* 현재 접촉 상태 조회 */
bool skin_contact_is_active(void);

#endif /* SKIN_CONTACT_H */
