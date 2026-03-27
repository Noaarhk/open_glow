/*
 * OpenGlow 상태 머신 (FSM) 구현
 *
 * switch-case 기반 FSM.
 * 각 상태에 on_enter / on_update / on_exit 로직.
 * event_queue에서 이벤트를 drain하며 상태 전이 수행.
 */

#include "device_fsm.h"
#include "event_queue.h"
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
            /* TODO: ems_stop(), vibration_stop() */
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
            /* TODO: led_breathe() */
            break;
        case STATE_MODE_SELECT:
            /* TODO: led_blink(), vibration_pulse() */
            break;
        case STATE_RUNNING:
            ctx.running_start_time_ms = get_time_ms();
            /* TODO: ems_set_mode(), ems_start(), led_set_mode_color() */
            break;
        case STATE_PAUSED:
            /* TODO: ems_stop(), led_blink_slow() */
            break;
        case STATE_CHARGING:
            /* TODO: ems_stop(), led_show_battery_level() */
            break;
        case STATE_ERROR:
            /* TODO: ems_stop(), led_blink_error(), vibration_pulse(3) */
            break;
        case STATE_SHUTDOWN:
            /* TODO: ems_stop(), led_fade_out(), vibration_pulse(1) */
            break;
        case STATE_OFF:
            /* TODO: deep_sleep() */
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
            /* 모드 순환: BOOSTER → MC → EMS → AIRSHOT → BOOSTER */
            ctx.current_mode = (ctx.current_mode + 1) % MODE_COUNT;
            ctx.last_interaction_ms = get_time_ms();
            LOG_INFO("Mode changed to %d", ctx.current_mode);
            /* TODO: led_set_mode_color(), vibration_pulse(1) */
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
            /* TODO: ems_set_intensity(), vibration_pulse() */
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
        case EVENT_SAFETY_TEMP_CRITICAL:
        case EVENT_SAFETY_BATTERY_CRITICAL:
            ctx.error_code = (evt->type == EVENT_SAFETY_TEMP_WARNING) ?
                             ERR_TEMP_WARNING : ERR_TEMP_CRITICAL;
            fsm_transition(STATE_ERROR);
            break;
        case EVENT_SAFETY_AUTO_TIMEOUT:
            fsm_transition(STATE_SHUTDOWN);
            break;
        case EVENT_SAFETY_AUTO_WARNING:
            /* TODO: vibration_pulse(2) — 8분 경고 */
            LOG_INFO("Auto timeout warning (8min)");
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
            fsm_transition(STATE_RUNNING);
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
            /* 온도 복구 체크: TEMP_WARNING이고 충분히 시간 경과 시 IDLE로 복구 */
            if (ctx.error_code == ERR_TEMP_WARNING &&
                elapsed >= SAFETY_TEMP_RECOVER_DELAY_MS) {
                /* TODO: 실제로는 battery_get_temperature()로 온도 확인 필요 */
                LOG_INFO("ERROR auto recovery (temp normalized)");
                ctx.error_code = ERR_NONE;
                fsm_transition(STATE_IDLE);
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
        .current_mode       = MODE_BOOSTER,
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
