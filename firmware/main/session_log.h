#ifndef SESSION_LOG_H
#define SESSION_LOG_H

/*
 * 세션 로그 모듈
 *
 * RUNNING 상태 동안의 사용 데이터를 추적하고,
 * 세션 종료 시 NVS에 저장. 최근 10개 세션을 링 버퍼 방식으로 보관.
 *
 * BLE 연결이 없어도 데이터 유실 없음.
 * 다음 BLE 연결 시 앱에서 Read로 조회 가능.
 *
 * 데이터 구조 (18 bytes, BLE 기본 MTU 20 bytes 이내):
 *   session_id     — 누적 세션 번호
 *   start_time     — 부팅 후 경과 시간 (초)
 *   duration_sec   — 총 사용 시간 (초)
 *   mode           — 주로 사용한 모드
 *   avg_intensity  — 평균 세기
 *   shot_count     — 총 샷(세기 변경) 횟수
 *   max_temp       — 최고 온도 (°C)
 *   min_battery    — 최저 배터리 (%)
 */

#include <stdbool.h>
#include <stdint.h>

/* NVS에 저장할 최대 세션 수 (링 버퍼) */
#define SESSION_LOG_MAX_COUNT   10

/* 세션 로그 데이터 구조 (packed = 18 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t session_id;        /* 누적 세션 번호 (1부터 시작) */
    uint32_t start_time;        /* 세션 시작 시각 (부팅 후 초) */
    uint32_t duration_sec;      /* 총 사용 시간 (초) */
    uint8_t  mode;              /* 주로 사용한 모드 */
    uint8_t  avg_intensity;     /* 평균 세기 (1~5) */
    uint16_t shot_count;        /* 총 샷 수 (세기 변경 횟수) */
    uint8_t  max_temp;          /* 최고 온도 (°C, 정수) */
    uint8_t  min_battery;       /* 최저 배터리 (%) */
} session_log_t;                /* 4+4+4+1+1+2+1+1 = 18 bytes (MTU 20 이내) */

/* 초기화 (NVS에서 세션 카운트 로드) */
void session_log_init(void);

/* 세션 시작 (FSM이 RUNNING 진입 시 호출) */
void session_log_start(uint8_t mode, uint8_t intensity);

/* 세션 중 업데이트 (메인 루프에서 호출, RUNNING 상태일 때만) */
void session_log_update(uint8_t mode, uint8_t intensity,
                        uint8_t temp, uint8_t battery);

/* 세기 변경 시 호출 (shot_count 증가) */
void session_log_add_shot(void);

/* 세션 종료 (FSM이 RUNNING 이탈 시 호출, NVS에 저장) */
void session_log_end(void);

/* 최근 세션 로그 조회 (BLE Read용, 없으면 false) */
bool session_log_get_latest(session_log_t *log);

/* NVS에 저장된 세션 수 조회 */
uint32_t session_log_get_count(void);

/* 세션 진행 중 여부 */
bool session_log_is_active(void);

/* 현재 진행 중인 세션 데이터 스냅샷 (BLE Notify용) */
bool session_log_get_current(session_log_t *log);

#endif /* SESSION_LOG_H */
