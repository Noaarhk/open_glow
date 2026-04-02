/*
 * 진동 모터 컨트롤러 구현
 *
 * hal_pwm의 PWM_CH_VIBRATION 채널 사용.
 * 20kHz PWM으로 coin motor를 구동.
 *
 * pulse 동작:
 *   vibration_pulse(200, 100, 3) 호출 시:
 *   → pulse_remaining = 3, pulse_is_on = true
 *   → update()에서 200ms 후 OFF, 100ms 후 ON, ... 반복
 *   → remaining이 0이 되면 자동 종료
 */

#include "vibration_controller.h"
#include "openglow_config.h"
#include "hal/hal_pwm.h"
#include "hal/hal_timer.h"
#include "debug_log.h"

static const char *TAG = "VIB";

/* 미사용 — vibration_set_intensity()와 함께 Phase 5 활성화 예정
static const uint8_t duty_table[5] = { 30, 45, 60, 75, 90 };
*/

/* 내부 상태 */
static struct {
    uint32_t current_duty;      /* 현재 LEDC 듀티 값 */

    /* 펄스 패턴 상태 */
    bool pulse_active;          /* 펄스 진행 중 */
    bool pulse_is_on;           /* 현재 ON 구간인지 */
    uint16_t pulse_on_ms;
    uint16_t pulse_off_ms;
    uint8_t pulse_remaining;    /* 남은 반복 횟수 */
    uint32_t pulse_next_ms;     /* 다음 전환 시각 */
} ctx;

/* 듀티비(%)를 LEDC raw 값으로 변환 */
static uint32_t pct_to_duty(uint8_t pct)
{
    uint32_t max_duty = (1 << VIBRATION_PWM_RESOLUTION_BITS) - 1;  /* 1023 */
    return max_duty * pct / 100;
}

void vibration_init(void)
{
    /* PWM 채널 설정: GPIO19, 20kHz, 10bit */
    hal_pwm_configure(PWM_CH_VIBRATION, PIN_VIBRATION_PWM,
                      VIBRATION_PWM_FREQ_HZ, VIBRATION_PWM_RESOLUTION_BITS);

    ctx.current_duty = pct_to_duty(VIBRATION_MIN_DUTY_PCT);  /* 기본 세기 1 */
    ctx.pulse_active = false;

    LOG_INFO("Vibration controller initialized (GPIO%d, %dHz)",
             PIN_VIBRATION_PWM, VIBRATION_PWM_FREQ_HZ);
}

/* 미사용 — Phase 5 연속 진동 모드 시 활성화 예정
void vibration_set_intensity(uint8_t level)
{
    if (level < 1) level = 1;
    if (level > 5) level = 5;

    ctx.current_duty = pct_to_duty(duty_table[level - 1]);
}
*/

void vibration_pulse(uint16_t on_ms, uint16_t off_ms, uint8_t count)
{
    if (count == 0) return;

    ctx.pulse_on_ms = on_ms;
    ctx.pulse_off_ms = off_ms;
    ctx.pulse_remaining = count;
    ctx.pulse_active = true;

    /* 첫 ON 시작 */
    ctx.pulse_is_on = true;
    ctx.pulse_next_ms = hal_timer_get_ms() + on_ms;
    hal_pwm_set_duty(PWM_CH_VIBRATION, ctx.current_duty);
    hal_pwm_start(PWM_CH_VIBRATION);
    LOG_INFO("VIB pulse START: duty=%lu, on=%dms, count=%d",
             (unsigned long)ctx.current_duty, on_ms, count);
}

/* 미사용 — Phase 5 연속 진동 모드 시 활성화 예정
void vibration_start(void)
{
    ctx.pulse_active = false;
    hal_pwm_set_duty(PWM_CH_VIBRATION, ctx.current_duty);
    hal_pwm_start(PWM_CH_VIBRATION);
}
*/

void vibration_stop(void)
{
    ctx.pulse_active = false;
    ctx.pulse_remaining = 0;
    hal_pwm_stop(PWM_CH_VIBRATION);
}

void vibration_update(void)
{
    if (!ctx.pulse_active) return;

    uint32_t now = hal_timer_get_ms();
    if (now < ctx.pulse_next_ms) return;

    if (ctx.pulse_is_on) {
        /* ON→OFF 전환 */
        hal_pwm_stop(PWM_CH_VIBRATION);
        ctx.pulse_is_on = false;
        ctx.pulse_remaining--;

        if (ctx.pulse_remaining == 0) {
            /* 모든 펄스 완료 */
            ctx.pulse_active = false;
            return;
        }
        ctx.pulse_next_ms = now + ctx.pulse_off_ms;
    } else {
        /* OFF→ON 전환 */
        hal_pwm_set_duty(PWM_CH_VIBRATION, ctx.current_duty);
        hal_pwm_start(PWM_CH_VIBRATION);
        ctx.pulse_is_on = true;
        ctx.pulse_next_ms = now + ctx.pulse_on_ms;
    }
}
