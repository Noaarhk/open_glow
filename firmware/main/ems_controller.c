/*
 * EMS 컨트롤러 구현
 *
 * 핵심 개념 (Django 비유):
 *   모드 = 이메일 템플릿 (PULSE, MICRO, EMS, THERMAL)
 *   주파수 = 발송 빈도 (얼마나 빨리 on/off를 반복하는지)
 *   듀티비 = 한 주기에서 ON이 차지하는 비율 (높을수록 강함)
 *
 *   예) 3kHz, 듀티 50% → 1초에 3000번, 각 on/off가 반반
 *
 * MICRO 모드 (10Hz) 특별 처리:
 *   ESP32 LEDC의 최소 주파수는 ~1220Hz (13bit 해상도).
 *   10Hz는 LEDC로 불가능 → 소프트웨어 타이머로 GPIO를 직접 토글.
 *   ems_update()에서 매 루프(10ms)마다 시간을 체크하여 on/off 전환.
 *
 *   10Hz = 100ms 주기
 *   듀티 30% → 30ms ON, 70ms OFF
 */

#include "ems_controller.h"
#include "hal/hal_pwm.h"
#include "hal/hal_gpio.h"
#include "hal/hal_timer.h"
#include "debug_log.h"

static const char *TAG = "EMS";

/* 모드별 듀티비 범위 (퍼센트, 정수) */
typedef struct {
    uint32_t freq_hz;       /* PWM 주파수 */
    uint8_t  duty_min_pct;  /* 세기 1의 듀티비 (%) */
    uint8_t  duty_max_pct;  /* 세기 5의 듀티비 (%) */
} mode_params_t;

/* 설계문서 기준 모드별 파라미터 */
static const mode_params_t mode_table[MODE_COUNT] = {
    [MODE_PULSE]   = { .freq_hz = EMS_PULSE_FREQ_HZ,   .duty_min_pct = 10, .duty_max_pct = 50 },
    [MODE_MICRO]   = { .freq_hz = EMS_MICRO_FREQ_HZ,   .duty_min_pct =  5, .duty_max_pct = 30 },
    [MODE_EMS]     = { .freq_hz = EMS_EMS_FREQ_HZ,     .duty_min_pct = 15, .duty_max_pct = 60 },
    [MODE_THERMAL] = { .freq_hz = EMS_THERMAL_FREQ_HZ,  .duty_min_pct = 10, .duty_max_pct = 45 },
};

/* 내부 상태 */
static struct {
    device_mode_t mode;
    uint8_t intensity;          /* 1~5 */
    float output_limit;         /* 0.0~1.0 (safety 제한) */
    bool active;                /* 출력 중 여부 */
    uint32_t target_duty;       /* 현재 목표 듀티 (LEDC raw 값) */

    /* MICRO 모드 소프트웨어 토글 상태 */
    bool mc_pin_high;           /* 현재 GPIO 상태 */
    uint32_t mc_toggle_time_ms; /* 다음 토글 시각 */
    uint32_t mc_on_time_ms;     /* ON 구간 길이 */
    uint32_t mc_off_time_ms;    /* OFF 구간 길이 */
} ctx;

/* 세기(1~5)를 LEDC 듀티 raw 값으로 변환 */
static uint32_t intensity_to_duty(uint8_t level)
{
    if (level < 1) level = 1;
    if (level > 5) level = 5;

    const mode_params_t *p = &mode_table[ctx.mode];

    /* 선형 보간: level 1 → duty_min, level 5 → duty_max */
    float pct = p->duty_min_pct +
                (p->duty_max_pct - p->duty_min_pct) * (level - 1) / 4.0f;

    /* output_limit 적용 (safety_manager가 0.0~1.0으로 제한) */
    pct *= ctx.output_limit;

    /* 퍼센트 → LEDC raw 값 (13bit: 0~8191) */
    uint32_t max_duty = (1 << EMS_PWM_RESOLUTION_BITS) - 1;  /* 8191 */
    return (uint32_t)(max_duty * pct / 100.0f);
}

/* MICRO 모드의 on/off 시간 계산 */
static void mc_calculate_timing(void)
{
    const mode_params_t *p = &mode_table[MODE_MICRO];
    float pct = p->duty_min_pct +
                (p->duty_max_pct - p->duty_min_pct) * (ctx.intensity - 1) / 4.0f;
    pct *= ctx.output_limit;

    /* 10Hz = 100ms 주기 */
    uint32_t period_ms = 1000 / EMS_MICRO_FREQ_HZ;  /* 100ms */
    ctx.mc_on_time_ms  = (uint32_t)(period_ms * pct / 100.0f);
    ctx.mc_off_time_ms = period_ms - ctx.mc_on_time_ms;

    /* 최소 ON 시간 보장 (1ms) */
    if (ctx.mc_on_time_ms < 1) ctx.mc_on_time_ms = 1;
}

void ems_init(void)
{
    ctx.mode = MODE_PULSE;
    ctx.intensity = 1;
    ctx.output_limit = 1.0f;
    ctx.active = false;
    ctx.target_duty = 0;
    ctx.mc_pin_high = false;

    /* EMS PWM 채널 초기 설정 (PULSE 모드 기본) */
    hal_pwm_configure(PWM_CH_EMS, PIN_EMS_PWM,
                      EMS_PULSE_FREQ_HZ, EMS_PWM_RESOLUTION_BITS);

    /* EMS ENABLE 핀: 출력 모드, 초기 LOW (차단 상태) */
    hal_gpio_set_output(PIN_EMS_ENABLE);
    hal_gpio_write(PIN_EMS_ENABLE, 0);

    LOG_INFO("EMS controller initialized (mode=PULSE, enable=OFF)");
}

void ems_set_mode(device_mode_t mode)
{
    if (mode >= MODE_COUNT) return;
    ctx.mode = mode;

    /* MICRO 모드가 아닌 경우: LEDC 주파수 재설정 */
    if (mode != MODE_MICRO) {
        hal_pwm_set_frequency(PWM_CH_EMS, mode_table[mode].freq_hz);
    }

    LOG_INFO("EMS mode set to %d (freq=%luHz)", mode,
             (unsigned long)mode_table[mode].freq_hz);
}

void ems_set_intensity(uint8_t level)
{
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    ctx.intensity = level;

    /* Phase 2: 즉시 전환 (Phase 5에서 PID 트랜지션으로 교체 예정) */
    ctx.target_duty = intensity_to_duty(level);

    if (ctx.active) {
        if (ctx.mode == MODE_MICRO) {
            mc_calculate_timing();
        } else {
            hal_pwm_set_duty(PWM_CH_EMS, ctx.target_duty);
        }
    }

    LOG_INFO("EMS intensity=%d, duty=%lu", level, (unsigned long)ctx.target_duty);
}

void ems_set_output_limit(float limit)
{
    if (limit < 0.0f) limit = 0.0f;
    if (limit > 1.0f) limit = 1.0f;
    ctx.output_limit = limit;

    /* 제한값이 바뀌면 듀티 재계산 */
    if (ctx.active) {
        ems_set_intensity(ctx.intensity);
    }
}

void ems_start(void)
{
    if (ctx.active) return;

    /* EMS ENABLE 핀 HIGH → 출력 회로 활성화 */
    hal_gpio_write(PIN_EMS_ENABLE, 1);

    ctx.target_duty = intensity_to_duty(ctx.intensity);
    ctx.active = true;

    if (ctx.mode == MODE_MICRO) {
        /* MICRO 모드: 소프트웨어 토글 시작 */
        mc_calculate_timing();
        ctx.mc_pin_high = true;
        ctx.mc_toggle_time_ms = hal_timer_get_ms() + ctx.mc_on_time_ms;
        hal_gpio_write(PIN_EMS_PWM, 1);
    } else {
        /* LEDC 모드: 하드웨어 PWM 시작 */
        hal_pwm_set_duty(PWM_CH_EMS, ctx.target_duty);
        hal_pwm_start(PWM_CH_EMS);
    }

    LOG_INFO("EMS started (mode=%d, intensity=%d, duty=%lu)",
             ctx.mode, ctx.intensity, (unsigned long)ctx.target_duty);
}

void ems_stop(void)
{
    if (!ctx.active) return;

    if (ctx.mode == MODE_MICRO) {
        hal_gpio_write(PIN_EMS_PWM, 0);
    } else {
        hal_pwm_stop(PWM_CH_EMS);
    }

    /* EMS ENABLE은 유지 (정상 종료에서는 바로 재시작 가능) */
    hal_gpio_write(PIN_EMS_ENABLE, 0);

    ctx.active = false;
    ctx.target_duty = 0;

    LOG_INFO("EMS stopped");
}

void ems_emergency_stop(void)
{
    /* 비상 차단: 소프트웨어 상태와 무관하게 즉시 하드웨어 OFF */
    hal_gpio_write(PIN_EMS_ENABLE, 0);  /* 하드웨어 레벨 차단 */
    hal_gpio_write(PIN_EMS_PWM, 0);     /* PWM 핀도 LOW */
    hal_pwm_stop(PWM_CH_EMS);           /* LEDC도 중지 */

    ctx.active = false;
    ctx.target_duty = 0;

    LOG_ERROR("EMS EMERGENCY STOP!");
}

void ems_update(void)
{
    if (!ctx.active) return;

    /* MICRO 모드: 소프트웨어 GPIO 토글 */
    if (ctx.mode == MODE_MICRO) {
        uint32_t now = hal_timer_get_ms();
        if (now >= ctx.mc_toggle_time_ms) {
            if (ctx.mc_pin_high) {
                /* ON→OFF 전환 */
                hal_gpio_write(PIN_EMS_PWM, 0);
                ctx.mc_pin_high = false;
                ctx.mc_toggle_time_ms = now + ctx.mc_off_time_ms;
            } else {
                /* OFF→ON 전환 */
                hal_gpio_write(PIN_EMS_PWM, 1);
                ctx.mc_pin_high = true;
                ctx.mc_toggle_time_ms = now + ctx.mc_on_time_ms;
            }
        }
    }

    /* TODO Phase 5: PID 업데이트 (현재는 즉시 전환이므로 처리 없음) */
}

/* 미사용 — Phase 5 BLE 출력값 보고 시 활성화 예정
float ems_get_current_duty(void)
{
    uint32_t max_duty = (1 << EMS_PWM_RESOLUTION_BITS) - 1;
    if (max_duty == 0) return 0.0f;
    return (float)ctx.target_duty / (float)max_duty;
}
*/

bool ems_is_active(void)
{
    return ctx.active;
}
