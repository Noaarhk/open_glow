/*
 * OpenGlow 상태 머신 (FSM) 구현
 *
 * switch-case 기반 FSM.
 * 각 상태에 on_enter / on_update / on_exit 로직.
 * event_queue에서 이벤트를 drain하며 상태 전이 수행.
 */

#include "device_fsm.h"
#include "event_queue.h"
#include "ems_controller.h"
#include "led_controller.h"
#include "vibration_controller.h"
#include "battery_monitor.h"
#include "session_log.h"
#include "debug_log.h"
#include "esp_timer.h"

static const char *TAG = "FSM";

static device_context_t ctx;

/* 현재 시각 (ms) */
static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 상태 이름 테이블 */
static const char *state_names[] = {
    "OFF", "IDLE", "MODE_SELECT", "RUNNING",
    "PAUSED", "CHARGING", "ERROR", "SHUTDOWN"
};

const char* fsm_state_name(device_state_t state)
{
    if (state < STATE_COUNT) {
        return state_names[state];
    }
    return "UNKNOWN";
}

/* === 상태 전이 함수 === */
static void fsm_transition(device_state_t new_state)
{
    if (new_state == ctx.current_state) {
        return;
    }

    LOG_INFO("%s -> %s", fsm_state_name(ctx.current_state), fsm_state_name(new_state));

    /* on_exit: 이전 상태 정리 */
    switch (ctx.current_state) {
        case STATE_RUNNING:
            ctx.total_session_time_ms += get_time_ms() - ctx.running_start_time_ms;
            /* PAUSED로 갈 때는 세션 유지 (재접촉 시 이어서 기록) */
            if (new_state != STATE_PAUSED) {
                session_log_end();  /* NVS에 세션 저장 */
            }
            ems_stop();
            vibration_stop();
            break;
        case STATE_PAUSED:
            /* PAUSED에서 RUNNING 복귀가 아니면 세션 종료 */
            if (new_state != STATE_RUNNING && session_log_is_active()) {
                session_log_end();
            }
            break;
        default:
            break;
    }

    /* 상태 변경 */
    ctx.previous_state = ctx.current_state;
    ctx.current_state = new_state;
    ctx.state_enter_time_ms = get_time_ms();

    /* on_enter: 새 상태 초기화 */
    switch (new_state) {
        case STATE_IDLE:
            ctx.last_interaction_ms = get_time_ms();
            led_set_mode_color(ctx.current_mode);
            led_breathe(LED_BREATHE_PERIOD_MS);
            if (ctx.previous_state == STATE_OFF) {
                vibration_pulse(200, 0, 1);  /* 전원 ON 햅틱 피드백 */
            }
            break;
        case STATE_MODE_SELECT:
            led_set_mode_color(ctx.current_mode);
            led_blink(LED_BLINK_FAST_MS, LED_BLINK_FAST_MS);
            vibration_pulse(100, 0, 1);  /* 햅틱 피드백 1회 */
            break;
        case STATE_RUNNING:
            ctx.running_start_time_ms = get_time_ms();
            /* PAUSED 복귀 시 세션 이어서 기록, 새 진입 시에만 시작 */
            if (!session_log_is_active()) {
                session_log_start(ctx.current_mode, ctx.intensity_level);
            }
            ems_set_mode(ctx.current_mode);
            ems_set_intensity(ctx.intensity_level);
            ems_start();
            led_set_mode_color(ctx.current_mode);
            /* 밝기 = 세기에 비례 (1→51, 2→102, 3→153, 4→204, 5→255) */
            led_set_brightness(ctx.intensity_level * 51);
            break;
        case STATE_PAUSED:
            ems_stop();
            led_blink(LED_BLINK_SLOW_MS, LED_BLINK_SLOW_MS);
            break;
        case STATE_CHARGING:
            ems_stop();
            led_show_battery_level(battery_get_percent());
            break;
        case STATE_ERROR:
            ems_stop();
            led_set_color(255, 0, 0);  /* 빨간색 */
            led_blink(100, 100);       /* 빠른 깜빡임 */
            vibration_pulse(200, 100, 3);  /* 강한 경고 3회 */
            break;
        case STATE_SHUTDOWN:
            ems_stop();
            led_fade_out(LED_FADE_OUT_MS);
            vibration_pulse(100, 0, 1);  /* 종료 알림 1회 */
            break;
        case STATE_OFF:
            led_off();
            /* TODO Phase 5: esp_deep_sleep_start() */
            break;
        default:
            break;
    }
}

/* === 상태별 이벤트 처리 === */

static void handle_off(event_t *evt)
{
    switch (evt->type) {
        case EVENT_BTN_POWER_LONG:
            fsm_transition(STATE_IDLE);
            break;
        case EVENT_CHARGE_CONNECTED:
            ctx.is_charging = true;
            fsm_transition(STATE_CHARGING);
            break;
        default:
            break;
    }
}

static void handle_idle(event_t *evt)
{
    switch (evt->type) {
        case EVENT_BTN_MODE_SHORT:
            ctx.last_interaction_ms = get_time_ms();
            fsm_transition(STATE_MODE_SELECT);
            break;
        case EVENT_BLE_MODE_CHANGE:
            ctx.current_mode = (device_mode_t)(evt->data % MODE_COUNT);
            ctx.last_interaction_ms = get_time_ms();
            LOG_INFO("BLE mode change to %d", ctx.current_mode);
            led_set_mode_color(ctx.current_mode);
            fsm_transition(STATE_MODE_SELECT);
            break;
        case EVENT_BTN_POWER_VLONG:
            fsm_transition(STATE_SHUTDOWN);
            break;
        case EVENT_CHARGE_CONNECTED:
            ctx.is_charging = true;
            fsm_transition(STATE_CHARGING);
            break;
        default:
            break;
    }
}

static void handle_mode_select(event_t *evt)
{
    switch (evt->type) {
        case EVENT_BTN_MODE_SHORT:
            /* 모드 순환: PULSE → MICRO → EMS → THERMAL → PULSE */
            ctx.current_mode = (ctx.current_mode + 1) % MODE_COUNT;
            ctx.last_interaction_ms = get_time_ms();
            LOG_INFO("Mode changed to %d", ctx.current_mode);
            led_set_mode_color(ctx.current_mode);
            vibration_pulse(100, 0, 1);
            break;
        case EVENT_BLE_MODE_CHANGE:
            ctx.current_mode = (device_mode_t)(evt->data % MODE_COUNT);
            ctx.last_interaction_ms = get_time_ms();
            LOG_INFO("BLE mode set to %d", ctx.current_mode);
            led_set_mode_color(ctx.current_mode);
            vibration_pulse(100, 0, 1);
            break;
        case EVENT_BTN_POWER_SHORT:
            /* 모드 확정, 실행 시작 */
            ctx.intensity_level = 1;  /* 세기 1부터 시작 (안전) */
            fsm_transition(STATE_RUNNING);
            break;
        case EVENT_BTN_POWER_VLONG:
            fsm_transition(STATE_SHUTDOWN);
            break;
        default:
            break;
    }
}

static void handle_running(event_t *evt)
{
    switch (evt->type) {
        case EVENT_BTN_MODE_SHORT:
            /* 세기 순환: 1→2→3→4→5→1 */
            ctx.intensity_level = (ctx.intensity_level % 5) + 1;
            ctx.last_interaction_ms = get_time_ms();
            LOG_INFO("Intensity changed to %d", ctx.intensity_level);
            ems_set_intensity(ctx.intensity_level);
            led_set_brightness(ctx.intensity_level * 51);
            vibration_pulse(50, 0, 1);
            session_log_add_shot();
            break;
        case EVENT_SKIN_CONTACT_OFF:
            ctx.skin_contact = false;
            fsm_transition(STATE_PAUSED);
            break;
        case EVENT_CHARGE_CONNECTED:
            ctx.is_charging = true;
            fsm_transition(STATE_CHARGING);
            break;
        case EVENT_BTN_POWER_VLONG:
            fsm_transition(STATE_SHUTDOWN);
            break;
        case EVENT_SAFETY_TEMP_WARNING:
            ctx.error_code = ERR_TEMP_WARNING;
            fsm_transition(STATE_ERROR);
            break;
        case EVENT_SAFETY_TEMP_CRITICAL:
            ctx.error_code = ERR_TEMP_CRITICAL;
            fsm_transition(STATE_ERROR);
            break;
        case EVENT_SAFETY_BATTERY_CRITICAL:
            ctx.error_code = ERR_BATTERY_CRITICAL;
            fsm_transition(STATE_ERROR);
            break;
        case EVENT_SAFETY_AUTO_TIMEOUT:
            fsm_transition(STATE_SHUTDOWN);
            break;
        case EVENT_SAFETY_AUTO_WARNING:
            vibration_pulse(150, 100, 2);  /* 8분 경고: 약한 진동 2회 */
            LOG_INFO("Auto timeout warning (8min)");
            break;
        case EVENT_BLE_INTENSITY_CHANGE: {
            uint8_t level = (uint8_t)evt->data;
            if (level >= 1 && level <= 5) {
                ctx.intensity_level = level;
                ctx.last_interaction_ms = get_time_ms();
                LOG_INFO("BLE intensity set to %d", level);
                ems_set_intensity(ctx.intensity_level);
                led_set_brightness(ctx.intensity_level * 51);
                vibration_pulse(50, 0, 1);
                session_log_add_shot();
            }
            break;
        }
        case EVENT_BLE_MODE_CHANGE:
            /* RUNNING 중 모드 변경 거부 — Notify로 현재 값 재전송됨 */
            LOG_INFO("BLE mode change rejected (RUNNING)");
            break;
        case EVENT_SAFETY_BATTERY_LOW:
            /* 배터리 10% 이하 경고 — LED + 진동 */
            led_set_color(255, 100, 0);  /* 주황색 경고 */
            led_blink(LED_BLINK_SLOW_MS, LED_BLINK_SLOW_MS);
            vibration_pulse(100, 100, 2);
            LOG_WARN("Battery low warning");
            break;
        default:
            break;
    }
}

static void handle_paused(event_t *evt)
{
    switch (evt->type) {
        case EVENT_SKIN_CONTACT_ON:
            ctx.skin_contact = true;
            /* 3초 이내 재접촉 → RUNNING 복귀, 3초 초과 → IDLE */
            if (get_time_ms() - ctx.state_enter_time_ms < PAUSE_RESUME_MS) {
                fsm_transition(STATE_RUNNING);
            } else {
                LOG_INFO("Skin contact after 3s, returning to IDLE");
                fsm_transition(STATE_IDLE);
            }
            break;
        case EVENT_BTN_POWER_SHORT:
            fsm_transition(STATE_IDLE);
            break;
        case EVENT_BTN_POWER_VLONG:
            fsm_transition(STATE_SHUTDOWN);
            break;
        default:
            break;
    }
}

static void handle_charging(event_t *evt)
{
    switch (evt->type) {
        case EVENT_CHARGE_DISCONNECTED:
            ctx.is_charging = false;
            fsm_transition(STATE_IDLE);
            break;
        case EVENT_CHARGE_COMPLETE:
            ctx.is_charging = false;
            fsm_transition(STATE_IDLE);
            break;
        default:
            /* CHARGING 상태에서는 전원 버튼 포함 대부분 무시 */
            break;
    }
}

static void handle_error(event_t *evt)
{
    switch (evt->type) {
        case EVENT_BTN_POWER_VLONG:
            fsm_transition(STATE_SHUTDOWN);
            break;
        default:
            break;
    }
}

static void handle_shutdown(event_t *evt)
{
    (void)evt;  /* SHUTDOWN에서는 이벤트 무시, 타임아웃으로 OFF 전이 */
}

/* === 타임아웃 체크 (on_update) === */
static void check_timeouts(void)
{
    uint32_t now = get_time_ms();
    uint32_t elapsed = now - ctx.state_enter_time_ms;

    switch (ctx.current_state) {
        case STATE_IDLE:
            /* 3분 무조작 → 자동 슬립 */
            if (now - ctx.last_interaction_ms >= IDLE_AUTO_SLEEP_MS) {
                LOG_INFO("IDLE auto sleep (3min)");
                fsm_transition(STATE_SHUTDOWN);
            }
            break;
        case STATE_MODE_SELECT:
            /* 10초 무조작 → IDLE 복귀 */
            if (now - ctx.last_interaction_ms >= MODE_SELECT_TIMEOUT_MS) {
                LOG_INFO("MODE_SELECT timeout (10s)");
                fsm_transition(STATE_IDLE);
            }
            break;
        case STATE_PAUSED:
            /* 10초 접촉 없음 → IDLE */
            if (elapsed >= PAUSE_TIMEOUT_MS) {
                LOG_INFO("PAUSED timeout (10s)");
                fsm_transition(STATE_IDLE);
            }
            break;
        case STATE_SHUTDOWN:
            /* 종료 시퀀스 완료 (1초 대기 후 OFF) */
            if (elapsed >= LED_FADE_OUT_MS) {
                fsm_transition(STATE_OFF);
            }
            break;
        case STATE_ERROR:
            /* 온도 복구 체크: TEMP_WARNING이고, 충분히 시간 경과하고, 실제 온도가 안전 범위일 때만 복구 */
            if (ctx.error_code == ERR_TEMP_WARNING &&
                elapsed >= SAFETY_TEMP_RECOVER_DELAY_MS) {
                float temp = battery_get_temperature();
                if (!battery_is_temp_connected() || temp <= SAFETY_TEMP_RECOVER_C) {
                    LOG_INFO("ERROR auto recovery (temp=%.1f°C <= %.0f°C)",
                             temp, SAFETY_TEMP_RECOVER_C);
                    ctx.error_code = ERR_NONE;
                    fsm_transition(STATE_IDLE);
                }
            }
            /* CRITICAL 에러는 자동 SHUTDOWN */
            if (ctx.error_code == ERR_TEMP_CRITICAL ||
                ctx.error_code == ERR_BATTERY_CRITICAL) {
                fsm_transition(STATE_SHUTDOWN);
            }
            break;
        default:
            break;
    }
}

/* === 공개 함수 === */

void fsm_init(void)
{
    ctx = (device_context_t){
        .current_state      = STATE_OFF,
        .previous_state     = STATE_OFF,
        .current_mode       = MODE_PULSE,
        .intensity_level    = 1,
        .running_start_time_ms = 0,
        .total_session_time_ms = 0,
        .state_enter_time_ms = get_time_ms(),
        .last_interaction_ms = get_time_ms(),
        .skin_contact       = false,
        .is_charging        = false,
        .error_code         = ERR_NONE,
    };
    LOG_INFO("FSM initialized (state=OFF)");
}

void fsm_update(void)
{
    /* 1. event_queue drain: 모든 대기 이벤트 처리 */
    event_t evt;
    while (event_queue_pop(&evt)) {
        /* 상태 무관 공통 이벤트 처리 */
        if (evt.type == EVENT_BLE_LED_COLOR_CHANGE) {
            uint8_t r = (evt.data >> 16) & 0xFF;
            uint8_t g = (evt.data >> 8) & 0xFF;
            uint8_t b = evt.data & 0xFF;
            led_set_color(r, g, b);
            LOG_INFO("BLE LED color set: R=%d G=%d B=%d", r, g, b);
            continue;
        }

        switch (ctx.current_state) {
            case STATE_OFF:         handle_off(&evt);         break;
            case STATE_IDLE:        handle_idle(&evt);        break;
            case STATE_MODE_SELECT: handle_mode_select(&evt); break;
            case STATE_RUNNING:     handle_running(&evt);     break;
            case STATE_PAUSED:      handle_paused(&evt);      break;
            case STATE_CHARGING:    handle_charging(&evt);    break;
            case STATE_ERROR:       handle_error(&evt);       break;
            case STATE_SHUTDOWN:    handle_shutdown(&evt);    break;
            default:
                LOG_ERROR("Unknown state: %d", ctx.current_state);
                break;
        }
    }

    /* 2. 타임아웃 체크 (이벤트와 무관하게 매 루프 실행) */
    check_timeouts();
}

device_state_t fsm_get_state(void)
{
    return ctx.current_state;
}

const device_context_t* fsm_get_context(void)
{
    return &ctx;
}
