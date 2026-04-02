/*
 * 피부 접촉 감지 구현 (TTP223 터치 센서)
 *
 * 동작 원리:
 *   TTP223은 손가락이 센서에 닿으면 정전용량 변화를 감지하여
 *   출력 핀을 HIGH로 올림. 손가락이 떨어지면 LOW.
 *
 *   ESP32는 이 디지털 신호를 GPIO32로 읽고,
 *   100ms 디바운스 후 상태 변화를 이벤트로 변환.
 *
 * 디바운스:
 *   TTP223 자체에도 디바운스가 있지만, 추가 소프트웨어 디바운스로
 *   브레드보드 환경의 노이즈/떨림을 한 번 더 걸러줌.
 */

#include "skin_contact.h"
#include "openglow_config.h"
#include "event_queue.h"
#include "hal/hal_gpio.h"
#include "hal/hal_timer.h"
#include "debug_log.h"

static const char *TAG = "SKIN";

static struct {
    bool current_state;         /* 현재 확정된 접촉 상태 */
    bool raw_state;             /* 디바운스 전 GPIO 읽기 값 */
    uint32_t last_change_ms;    /* raw 상태가 변한 시각 */
} ctx;

void skin_contact_init(void)
{
    /* TTP223 출력은 디지털 HIGH/LOW이므로 풀업 불필요 */
    hal_gpio_set_input(PIN_SKIN_CONTACT, false);

    ctx.current_state = false;
    ctx.raw_state = false;
    ctx.last_change_ms = 0;

    LOG_INFO("Skin contact initialized (GPIO%d, active=%s)",
             PIN_SKIN_CONTACT,
             SKIN_CONTACT_ACTIVE_LEVEL ? "HIGH" : "LOW");
}

void skin_contact_update(void)
{
    /* GPIO 읽기: ACTIVE_LEVEL과 비교하여 접촉 여부 판단 */
    bool reading = (hal_gpio_read(PIN_SKIN_CONTACT) == SKIN_CONTACT_ACTIVE_LEVEL);

    /* raw 상태가 변했으면 디바운스 타이머 시작 */
    if (reading != ctx.raw_state) {
        ctx.raw_state = reading;
        ctx.last_change_ms = hal_timer_get_ms();
        return;
    }

    /* 디바운스 시간 경과 + 확정 상태와 다르면 → 상태 전이 */
    if (ctx.raw_state != ctx.current_state &&
        hal_timer_get_ms() - ctx.last_change_ms >= SKIN_CONTACT_DEBOUNCE_MS) {

        ctx.current_state = ctx.raw_state;

        event_t evt = {
            .data = 0,
            .timestamp_ms = hal_timer_get_ms(),
        };

        if (ctx.current_state) {
            evt.type = EVENT_SKIN_CONTACT_ON;
            LOG_INFO("Skin contact ON");
        } else {
            evt.type = EVENT_SKIN_CONTACT_OFF;
            LOG_INFO("Skin contact OFF");
        }

        event_queue_push(evt);
    }
}

/* 미사용 — FSM에서 피부 접촉 동기 조회 시 활성화 예정
bool skin_contact_is_active(void)
{
    return ctx.current_state;
}
*/
