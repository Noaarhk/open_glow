/*
 * OpenGlow 버튼 핸들러 구현
 *
 * 디바운싱: 10ms 주기로 GPIO를 읽어 5회 연속 동일하면 확정 (50ms)
 * 길게 누름: 누르는 도중 2초/3초 시점에 즉시 이벤트 발생 (해제 안 기다림)
 * 짧게 누름: 버튼 해제 시 발생 (2초 미만)
 */

#include "button_handler.h"
#include "openglow_config.h"
#include "event_queue.h"
#include "debug_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const char *TAG = "BTN";

#define DEBOUNCE_COUNT  (BTN_DEBOUNCE_MS / MAIN_LOOP_PERIOD_MS)  /* 50ms / 10ms = 5 */

/* 버튼 상태 구조체 */
typedef struct {
    gpio_num_t pin;
    bool current_state;             /* 디바운스 후 확정된 상태 (true=눌림) */
    bool last_raw;                  /* 이전 raw 읽기 값 */
    uint8_t debounce_counter;       /* 연속 동일 값 카운터 */
    uint32_t press_start_ms;        /* 눌림 시작 시각 */
    bool long_fired;                /* LONG 이벤트 발생 여부 */
    bool vlong_fired;               /* VLONG 이벤트 발생 여부 */
    event_type_t short_event;       /* 이 버튼의 SHORT 이벤트 */
    event_type_t long_event;        /* 이 버튼의 LONG 이벤트 */
    event_type_t vlong_event;       /* 이 버튼의 VLONG 이벤트 */
} button_t;

static button_t buttons[2];

static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void button_init(void)
{
    /* 전원 버튼 설정 */
    buttons[0] = (button_t){
        .pin           = PIN_BTN_POWER,
        .current_state = false,
        .last_raw      = false,
        .debounce_counter = 0,
        .long_fired    = false,
        .vlong_fired   = false,
        .short_event   = EVENT_BTN_POWER_SHORT,
        .long_event    = EVENT_BTN_POWER_LONG,
        .vlong_event   = EVENT_BTN_POWER_VLONG,
    };

    /* 모드 버튼 설정 */
    buttons[1] = (button_t){
        .pin           = PIN_BTN_MODE,
        .current_state = false,
        .last_raw      = false,
        .debounce_counter = 0,
        .long_fired    = false,
        .vlong_fired   = false,
        .short_event   = EVENT_BTN_MODE_SHORT,
        .long_event    = EVENT_BTN_MODE_LONG,
        .vlong_event   = EVENT_BTN_MODE_SHORT,  /* 모드 버튼은 VLONG 없음 */
    };

    /* GPIO 입력 + 풀업 설정 */
    for (int i = 0; i < 2; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << buttons[i].pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }

    LOG_INFO("Button handler initialized (POWER=GPIO%d, MODE=GPIO%d)",
             PIN_BTN_POWER, PIN_BTN_MODE);
}

/* 개별 버튼 업데이트 */
static void button_process(button_t *btn)
{
    /* 1. GPIO raw 읽기 (LOW=눌림, 풀업이므로) */
    bool raw_pressed = (gpio_get_level(btn->pin) == 0);

    /* 2. 디바운싱: 이전 값과 동일하면 카운터 증가 */
    if (raw_pressed == btn->last_raw) {
        if (btn->debounce_counter < DEBOUNCE_COUNT) {
            btn->debounce_counter++;
        }
    } else {
        btn->debounce_counter = 0;
    }
    btn->last_raw = raw_pressed;

    /* 카운터가 충분하지 않으면 아직 확정 안 됨 */
    if (btn->debounce_counter < DEBOUNCE_COUNT) {
        /* 디바운싱 중이라도, 이미 눌린 상태면 홀드 시간 체크 계속 */
        if (btn->current_state) {
            uint32_t held_ms = get_time_ms() - btn->press_start_ms;
            if (held_ms >= BTN_VLONG_MS && !btn->vlong_fired) {
                event_t evt = { .type = btn->vlong_event, .data = 0 };
                event_queue_push(evt);
                btn->vlong_fired = true;
                LOG_INFO("Button GPIO%d VLONG (%lums)", btn->pin, (unsigned long)held_ms);
            }
            else if (held_ms >= BTN_LONG_MS && !btn->long_fired) {
                event_t evt = { .type = btn->long_event, .data = 0 };
                event_queue_push(evt);
                btn->long_fired = true;
                LOG_INFO("Button GPIO%d LONG (%lums)", btn->pin, (unsigned long)held_ms);
            }
        }
        return;
    }

    /* 3. 상태 변화 감지 */
    bool prev_state = btn->current_state;
    btn->current_state = raw_pressed;

    if (raw_pressed && !prev_state) {
        /* === 눌림 시작 === */
        btn->press_start_ms = get_time_ms();
        btn->long_fired = false;
        btn->vlong_fired = false;
        LOG_DEBUG("Button GPIO%d pressed", btn->pin);
    }
    else if (raw_pressed && prev_state) {
        /* === 누르고 있는 중: 길게 누름 체크 === */
        uint32_t held_ms = get_time_ms() - btn->press_start_ms;

        if (held_ms >= BTN_VLONG_MS && !btn->vlong_fired) {
            /* 3초 이상: VLONG 이벤트 즉시 발생 */
            event_t evt = { .type = btn->vlong_event, .data = 0 };
            event_queue_push(evt);
            btn->vlong_fired = true;
            LOG_INFO("Button GPIO%d VLONG (%lums)", btn->pin, (unsigned long)held_ms);
        }
        else if (held_ms >= BTN_LONG_MS && !btn->long_fired) {
            /* 2초 이상: LONG 이벤트 즉시 발생 */
            event_t evt = { .type = btn->long_event, .data = 0 };
            event_queue_push(evt);
            btn->long_fired = true;
            LOG_INFO("Button GPIO%d LONG (%lums)", btn->pin, (unsigned long)held_ms);
        }
    }
    else if (!raw_pressed && prev_state) {
        /* === 버튼 해제 === */
        uint32_t held_ms = get_time_ms() - btn->press_start_ms;

        if (!btn->long_fired && !btn->vlong_fired && held_ms >= BTN_DEBOUNCE_MS) {
            /* LONG/VLONG이 발생하지 않았고, 디바운스 이상 눌렸으면 SHORT */
            event_t evt = { .type = btn->short_event, .data = 0 };
            event_queue_push(evt);
            LOG_INFO("Button GPIO%d SHORT (%lums)", btn->pin, (unsigned long)held_ms);
        }
    }
}

void button_update(void)
{
    for (int i = 0; i < 2; i++) {
        button_process(&buttons[i]);
    }
}
