/*
 * 안전 관리자 구현
 *
 * 센서 상태에 따른 3단계 안전 정책:
 *
 * 1. 센서 연결됨 + 데이터 유효 → 데이터 기반 정상 판단
 *    (과열 단계별 출력 제한, 저전압 차단 등)
 *
 * 2. 센서 연결됨 + 데이터 이상 → 센서 고장 → 비상 차단
 *    (부팅 때 연결되어 있었는데 동작 중 빠지거나 고장)
 *
 * 3. 센서 미연결 (부팅 시 감지) → 대체 안전 정책
 *    (출력 제한 + 동작 시간 축소, 센서 없이 무제한 동작 방지)
 *
 * 출력 제한(output_limit)은 FSM이 주기적으로 가져가서 EMS에 전달:
 *   fsm_update() → safety_get_output_limit() → ems_set_output_limit()
 *
 * 과열 출력 제한 그래프 (센서 연결 시):
 *   온도       출력 제한
 *   <40°C     1.0 (제한 없음)
 *   40~45°C   1.0→0.5 (선형 감소)
 *   45~50°C   0.5 (고정)
 *   ≥50°C     0.0 (비상 차단)
 */

#include "safety_manager.h"
#include "openglow_config.h"
#include "event_queue.h"
#include "battery_monitor.h"
#include "ems_controller.h"
#include "hal/hal_gpio.h"
#include "hal/hal_timer.h"
#include "debug_log.h"

static const char *TAG = "SAFE";

static struct {
    safety_status_t status;
    float output_limit;             /* 0.0~1.0 */
    uint32_t last_shot_ms;          /* 마지막 샷 시각 */
    uint32_t running_start_ms;      /* RUNNING 상태 시작 시각 */
    bool running_tracked;           /* RUNNING 타이머 활성화 여부 */
    bool warning_8min_sent;         /* 8분 경고 전송 여부 */
    bool emergency_triggered;       /* 비상 차단 발동 여부 */
} ctx;

/* 이벤트 push 헬퍼 */
static void push_event(event_type_t type)
{
    event_t evt = {
        .type = type,
        .data = 0,
        .timestamp_ms = hal_timer_get_ms(),
    };
    event_queue_push(evt);
}

/* === 온도 체크 === */
static void check_temperature(void)
{
    if (!battery_is_temp_connected()) {
        /* 센서 미연결: 대체 정책 — 출력 제한 */
        if (ctx.output_limit > SAFETY_NO_TEMP_OUTPUT_LIMIT) {
            ctx.output_limit = SAFETY_NO_TEMP_OUTPUT_LIMIT;
        }
        return;
    }

    /* 센서가 부팅 시 연결되어 있었는데, 지금 데이터가 이상하면 → 고장 */
    if (!battery_is_temp_valid()) {
        if (!ctx.emergency_triggered) {
            LOG_ERROR("Temp sensor FAULT (was connected, now invalid) → EMERGENCY SHUTDOWN");
            ctx.status = SAFETY_TEMP_CRITICAL;
            safety_emergency_shutdown();
            push_event(EVENT_SAFETY_TEMP_CRITICAL);
        }
        return;
    }

    /* 센서 연결 + 데이터 유효 → 정상 데이터 기반 판단 */
    float temp = battery_get_temperature();

    if (temp >= SAFETY_TEMP_CRITICAL_C) {
        /* Level 2: 비상 차단 (FSM 우회) */
        if (!ctx.emergency_triggered) {
            LOG_ERROR("CRITICAL: Temp %.1f°C >= %.0f°C → EMERGENCY SHUTDOWN",
                      temp, SAFETY_TEMP_CRITICAL_C);
            safety_emergency_shutdown();
            push_event(EVENT_SAFETY_TEMP_CRITICAL);
        }
        return;
    }

    if (temp >= SAFETY_TEMP_LIMIT_50_C) {
        /* 45~50°C: 출력 50% 제한 + 경고 */
        ctx.output_limit = 0.5f;
        if (ctx.status != SAFETY_TEMP_WARNING) {
            ctx.status = SAFETY_TEMP_WARNING;
            LOG_WARN("Temp %.1f°C: output limited to 50%%", temp);
            push_event(EVENT_SAFETY_TEMP_WARNING);
        }
    } else if (temp >= SAFETY_TEMP_WARNING_C) {
        /* 40~45°C: 선형 감소 (1.0→0.5) */
        float ratio = (temp - SAFETY_TEMP_WARNING_C) /
                      (SAFETY_TEMP_LIMIT_50_C - SAFETY_TEMP_WARNING_C);
        ctx.output_limit = 1.0f - (ratio * 0.5f);

        if (ctx.status == SAFETY_OK) {
            ctx.status = SAFETY_TEMP_WARNING;
            LOG_WARN("Temp %.1f°C: output limit %.0f%%", temp, ctx.output_limit * 100);
            push_event(EVENT_SAFETY_TEMP_WARNING);
        }
    } else {
        /* 40°C 미만: 정상 */
        if (ctx.status == SAFETY_TEMP_WARNING) {
            /* 히스테리시스: 38°C 이하로 내려와야 복구 */
            if (temp <= SAFETY_TEMP_RECOVER_C) {
                ctx.status = SAFETY_OK;
                ctx.output_limit = 1.0f;
                LOG_INFO("Temp normalized (%.1f°C)", temp);
            }
        } else {
            ctx.output_limit = 1.0f;
        }
    }
}

/* === 배터리 체크 === */
static void check_battery(void)
{
    if (!battery_is_voltage_connected()) {
        /* 센서 미연결: 대체 정책은 check_auto_timeout()에서 시간 제한으로 처리 */
        return;
    }

    /* 센서가 부팅 시 연결되어 있었는데, 지금 데이터가 이상하면 → 고장 */
    if (!battery_is_voltage_valid()) {
        if (!ctx.emergency_triggered) {
            LOG_ERROR("Battery sensor FAULT (was connected, now invalid) → EMERGENCY SHUTDOWN");
            ctx.status = SAFETY_BATTERY_CRITICAL;
            safety_emergency_shutdown();
            push_event(EVENT_SAFETY_BATTERY_CRITICAL);
        }
        return;
    }

    /* 센서 연결 + 데이터 유효 → 정상 데이터 기반 판단 */
    uint8_t percent = battery_get_percent();

    if (percent <= SAFETY_BATTERY_CRIT_PCT) {
        /* Level 2: 비상 차단 */
        if (!ctx.emergency_triggered) {
            LOG_ERROR("CRITICAL: Battery %d%% <= %d%% → EMERGENCY SHUTDOWN",
                      percent, SAFETY_BATTERY_CRIT_PCT);
            safety_emergency_shutdown();
            push_event(EVENT_SAFETY_BATTERY_CRITICAL);
        }
        return;
    }

    if (percent <= SAFETY_BATTERY_LOW_PCT) {
        /* 10% 이하: 출력 70% 제한 */
        if (ctx.output_limit > 0.7f) {
            ctx.output_limit = 0.7f;
        }
        if (ctx.status != SAFETY_BATTERY_LOW) {
            ctx.status = SAFETY_BATTERY_LOW;
            LOG_WARN("Battery low (%d%%): output limited to 70%%", percent);
            push_event(EVENT_SAFETY_BATTERY_LOW);
        }
    }
}

/* === 자동 꺼짐 타이머 === */
static void check_auto_timeout(void)
{
    /* FSM의 RUNNING 상태를 직접 알 수 없으므로
     * EMS 활성 상태를 기준으로 추적 */
    bool ems_active = ems_is_active();

    if (ems_active && !ctx.running_tracked) {
        /* RUNNING 시작 */
        ctx.running_start_ms = hal_timer_get_ms();
        ctx.running_tracked = true;
        ctx.warning_8min_sent = false;
    } else if (!ems_active && ctx.running_tracked) {
        /* RUNNING 종료 */
        ctx.running_tracked = false;
    }

    if (!ctx.running_tracked) return;

    uint32_t elapsed = hal_timer_get_ms() - ctx.running_start_ms;

    /* 센서 미연결 시 동작 시간 축소 */
    uint32_t max_ms = RUNNING_MAX_MS;
    if (!battery_is_temp_connected()) {
        max_ms = SAFETY_NO_TEMP_MAX_MS;  /* 온도 센서 없으면 3분 */
    }
    if (!battery_is_voltage_connected() && SAFETY_NO_BATT_MAX_MS < max_ms) {
        max_ms = SAFETY_NO_BATT_MAX_MS;  /* 배터리 센서 없으면 20분 (더 짧은 쪽 적용) */
    }
    uint32_t warning_ms = max_ms * 8 / 10;  /* 80% 지점에서 경고 */

    /* 자동 종료 */
    if (elapsed >= max_ms) {
        LOG_WARN("Auto timeout: %lu ms >= %lu ms%s",
                 (unsigned long)elapsed, (unsigned long)max_ms,
                 (max_ms < RUNNING_MAX_MS) ? " (sensor fallback)" : "");
        push_event(EVENT_SAFETY_AUTO_TIMEOUT);
        ctx.running_tracked = false;
        return;
    }

    /* 경고 */
    if (elapsed >= warning_ms && !ctx.warning_8min_sent) {
        LOG_INFO("Auto timeout warning (%lus / %lus)",
                 (unsigned long)(elapsed / 1000), (unsigned long)(max_ms / 1000));
        push_event(EVENT_SAFETY_AUTO_WARNING);
        ctx.warning_8min_sent = true;
    }
}

/* === 공개 함수 === */

void safety_init(void)
{
    ctx.status = SAFETY_OK;
    ctx.output_limit = 1.0f;
    ctx.last_shot_ms = 0;
    ctx.running_start_ms = 0;
    ctx.running_tracked = false;
    ctx.warning_8min_sent = false;
    ctx.emergency_triggered = false;

    LOG_INFO("Safety manager initialized");
}

void safety_update(void)
{
    /* 비상 차단이 발동되면 더 이상 체크하지 않음 */
    if (ctx.emergency_triggered) return;

    check_temperature();
    check_battery();
    check_auto_timeout();
}

safety_status_t safety_get_status(void)
{
    return ctx.status;
}

bool safety_can_fire_shot(void)
{
    return (hal_timer_get_ms() - ctx.last_shot_ms) >= SAFETY_SHOT_COOLDOWN_MS;
}

void safety_record_shot(void)
{
    ctx.last_shot_ms = hal_timer_get_ms();
}

float safety_get_output_limit(void)
{
    return ctx.output_limit;
}

void safety_emergency_shutdown(void)
{
    ctx.emergency_triggered = true;
    ctx.status = SAFETY_EMERGENCY_SHUTDOWN;
    ctx.output_limit = 0.0f;

    /* Level 2: FSM 우회, 하드웨어 직접 차단 */
    hal_gpio_write(PIN_EMS_ENABLE, 0);  /* EMS ENABLE 핀 강제 LOW */
    ems_emergency_stop();                /* EMS PWM도 중지 */

    LOG_ERROR("=== EMERGENCY SHUTDOWN ACTIVATED ===");
}
