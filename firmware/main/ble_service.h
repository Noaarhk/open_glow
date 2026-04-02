#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

/*
 * BLE GATT 서버 모듈
 *
 * ESP-IDF Bluedroid 스택 사용.
 * OpenGlow 커스텀 서비스 1개 + Characteristic 9개 구성.
 *
 * BLE 콜백은 별도 태스크에서 실행됨 → FSM 직접 호출 금지.
 * Write 요청은 event_queue에 push하여 메인 루프에서 처리.
 *
 * ble_update()에서 상태 변화 감지 → Notify 전송.
 */

#include <stdbool.h>
#include <stdint.h>

/* 초기화 (GAP/GATT 등록, 광고 시작) */
void ble_init(void);

/* 메인 루프에서 호출 (상태 변화 → Notify 전송) */
void ble_update(void);

/* BLE 연결 여부 */
bool ble_is_connected(void);

#endif /* BLE_SERVICE_H */
