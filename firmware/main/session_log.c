/*
 * 세션 로그 모듈 구현
 *
 * NVS 저장 구조:
 *   "sess_cnt"   → uint32_t: 누적 세션 수
 *   "sess_idx"   → uint32_t: 링 버퍼 현재 인덱스 (0~9)
 *   "sess_0" ~ "sess_9" → blob: session_log_t (최근 10개)
 *
 * 링 버퍼 방식:
 *   새 세션 저장 시 sess_idx 위치에 덮어쓰고, sess_idx를 (idx+1) % 10으로 갱신.
 *   가장 최근 세션은 (sess_idx - 1 + 10) % 10 위치.
 *
 * 평균 세기 계산:
 *   누적합 / 업데이트 횟수로 계산. 정수 나눗셈 (소수점 버림).
 *
 * 주 모드 결정:
 *   각 모드별 사용 횟수(tick)를 카운트하여 가장 많이 사용한 모드 선택.
 */

#include "session_log.h"
#include "hal/hal_nvs.h"
#include "hal/hal_timer.h"
#include "debug_log.h"
#include "openglow_config.h"

#include <string.h>

static const char *TAG = "SESSION";

/* NVS 키 */
#define KEY_SESSION_COUNT   "sess_cnt"
#define KEY_SESSION_INDEX   "sess_idx"
#define KEY_PREFIX          "sess_"     /* sess_0 ~ sess_9 */

/* 내부 상태 */
static struct {
    bool active;                        /* 세션 진행 중 여부 */
    uint32_t total_count;               /* NVS에서 로드한 누적 세션 수 */
    uint32_t ring_index;                /* NVS 링 버퍼 인덱스 */

    /* 현재 세션 추적 데이터 */
    session_log_t current;              /* 현재 세션 로그 */
    uint32_t intensity_sum;             /* 세기 누적합 (평균 계산용) */
    uint32_t update_count;              /* 업데이트 횟수 */
    uint32_t mode_ticks[MODE_COUNT];    /* 모드별 사용 틱 수 */
} state;

/* NVS 키 생성 헬퍼: "sess_0" ~ "sess_9" */
static void make_slot_key(uint32_t idx, char *buf, size_t buf_size)
{
    /* snprintf 대신 수동 생성 (최소 의존성) */
    (void)buf_size;
    buf[0] = 's'; buf[1] = 'e'; buf[2] = 's'; buf[3] = 's';
    buf[4] = '_'; buf[5] = '0' + (char)(idx % 10); buf[6] = '\0';
}

void session_log_init(void)
{
    memset(&state, 0, sizeof(state));

    /* NVS에서 누적 세션 수 로드 */
    if (!hal_nvs_get_u32(KEY_SESSION_COUNT, &state.total_count)) {
        state.total_count = 0;  /* 첫 실행: 세션 0개 */
    }

    /* NVS에서 링 버퍼 인덱스 로드 */
    if (!hal_nvs_get_u32(KEY_SESSION_INDEX, &state.ring_index)) {
        state.ring_index = 0;
    }

    LOG_INFO("Session log initialized (total=%lu, ring_idx=%lu)",
             (unsigned long)state.total_count,
             (unsigned long)state.ring_index);
}

void session_log_start(uint8_t mode, uint8_t intensity)
{
    if (state.active) {
        LOG_WARN("Session already active, ending previous");
        session_log_end();
    }

    state.active = true;
    state.intensity_sum = 0;
    state.update_count = 0;
    memset(state.mode_ticks, 0, sizeof(state.mode_ticks));

    /* 현재 세션 초기화 */
    state.current.session_id = state.total_count + 1;
    state.current.start_time = hal_timer_get_ms() / 1000;  /* 부팅 후 초 */
    state.current.duration_sec = 0;
    state.current.mode = mode;
    state.current.avg_intensity = intensity;
    state.current.shot_count = 0;
    state.current.max_temp = 0;
    state.current.min_battery = 100;

    LOG_INFO("Session #%lu started (mode=%d, intensity=%d)",
             (unsigned long)state.current.session_id, mode, intensity);
}

void session_log_update(uint8_t mode, uint8_t intensity,
                        uint8_t temp, uint8_t battery)
{
    if (!state.active) return;

    /* 경과 시간 갱신 */
    uint32_t now_sec = hal_timer_get_ms() / 1000;
    state.current.duration_sec = now_sec - state.current.start_time;

    /* 세기 누적 (평균 계산용) */
    state.intensity_sum += intensity;
    state.update_count++;
    state.current.avg_intensity = (uint8_t)(state.intensity_sum / state.update_count);

    /* 모드별 사용 틱 카운트 */
    if (mode < MODE_COUNT) {
        state.mode_ticks[mode]++;
    }

    /* 최고 온도 갱신 */
    if (temp > state.current.max_temp) {
        state.current.max_temp = temp;
    }

    /* 최저 배터리 갱신 */
    if (battery < state.current.min_battery) {
        state.current.min_battery = battery;
    }
}

void session_log_add_shot(void)
{
    if (!state.active) return;
    state.current.shot_count++;
}

void session_log_end(void)
{
    if (!state.active) return;

    /* 최종 경과 시간 확정 */
    uint32_t now_sec = hal_timer_get_ms() / 1000;
    state.current.duration_sec = now_sec - state.current.start_time;

    /* 주 모드 결정: 가장 많이 사용한 모드 */
    uint32_t max_ticks = 0;
    for (int i = 0; i < MODE_COUNT; i++) {
        if (state.mode_ticks[i] > max_ticks) {
            max_ticks = state.mode_ticks[i];
            state.current.mode = (uint8_t)i;
        }
    }

    /* NVS에 저장 */
    char key[8];
    make_slot_key(state.ring_index, key, sizeof(key));

    bool saved = hal_nvs_set_blob(key, &state.current, sizeof(session_log_t));
    if (saved) {
        /* 누적 카운트 증가 */
        state.total_count++;
        hal_nvs_set_u32(KEY_SESSION_COUNT, state.total_count);

        /* 링 버퍼 인덱스 전진 */
        state.ring_index = (state.ring_index + 1) % SESSION_LOG_MAX_COUNT;
        hal_nvs_set_u32(KEY_SESSION_INDEX, state.ring_index);

        LOG_INFO("Session #%lu saved (dur=%lus, mode=%d, avg_int=%d, shots=%d)",
                 (unsigned long)state.current.session_id,
                 (unsigned long)state.current.duration_sec,
                 state.current.mode,
                 state.current.avg_intensity,
                 state.current.shot_count);
    } else {
        LOG_ERROR("Session save failed!");
    }

    state.active = false;
}

bool session_log_get_latest(session_log_t *log)
{
    if (state.total_count == 0) return false;

    /* 가장 최근 세션 = (ring_index - 1 + 10) % 10 */
    uint32_t latest_idx = (state.ring_index + SESSION_LOG_MAX_COUNT - 1)
                          % SESSION_LOG_MAX_COUNT;
    char key[8];
    make_slot_key(latest_idx, key, sizeof(key));

    size_t len = sizeof(session_log_t);
    return hal_nvs_get_blob(key, log, &len);
}

uint32_t session_log_get_count(void)
{
    return state.total_count;
}

bool session_log_is_active(void)
{
    return state.active;
}

bool session_log_get_current(session_log_t *log)
{
    if (!state.active) return false;

    /* 현재 진행 중인 세션의 스냅샷 */
    *log = state.current;

    /* 경과 시간은 최신값으로 갱신 */
    uint32_t now_sec = hal_timer_get_ms() / 1000;
    log->duration_sec = now_sec - state.current.start_time;

    return true;
}
