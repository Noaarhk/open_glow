#ifndef HAL_NVS_H
#define HAL_NVS_H

/*
 * NVS(Non-Volatile Storage) 하드웨어 추상화 레이어
 *
 * ESP32의 플래시 메모리에 데이터를 영구 저장.
 * 전원이 꺼져도 데이터가 유지됨 (Django의 DB와 비슷한 역할).
 *
 * 용도:
 *   - 세션 로그 저장 (Phase 4)
 *   - 에러 이력 저장
 *   - 사용자 설정 저장 (커스텀 LED 색상 등)
 *
 * NVS는 key-value 저장소:
 *   "session_count" → 42
 *   "last_error"    → 0x02
 *   "session_log"   → {blob 데이터}
 *
 * 사용 예:
 *   hal_nvs_set_u32("session_count", 42);
 *   uint32_t count;
 *   hal_nvs_get_u32("session_count", &count);
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NVS 초기화 (플래시 파티션 마운트) */
void hal_nvs_init(void);

/* 32비트 정수 저장 */
bool hal_nvs_set_u32(const char *key, uint32_t value);

/* 32비트 정수 읽기 (성공 시 true, 키 없으면 false) */
bool hal_nvs_get_u32(const char *key, uint32_t *value);

/* 바이너리 데이터(blob) 저장 (구조체 등) */
bool hal_nvs_set_blob(const char *key, const void *data, size_t len);

/* 바이너리 데이터(blob) 읽기 (len은 입력: 버퍼 크기, 출력: 실제 크기) */
bool hal_nvs_get_blob(const char *key, void *data, size_t *len);

#endif /* HAL_NVS_H */
