/*
 * PWM HAL 구현 (ESP32 LEDC)
 *
 * ESP-IDF의 LEDC 드라이버를 래핑.
 *
 * LEDC 동작 원리:
 *   타이머가 설정된 주파수로 카운터를 돌리고,
 *   채널이 카운터 값과 듀티 값을 비교하여 HIGH/LOW 출력.
 *   → 듀티가 클수록 HIGH 시간이 길어짐 (출력 세기 증가)
 *
 * 타이머-채널 매핑:
 *   채널 0 (EMS)       → 타이머 0 (모드별 주파수 변경 필요)
 *   채널 1 (VIBRATION) → 타이머 1 (20kHz 고정)
 */

#include "hal_pwm.h"
#include "../debug_log.h"
#include "driver/ledc.h"

static const char *TAG = "HAL_PWM";

/* 채널별 설정 정보 저장 (주파수 변경 시 필요) */
typedef struct {
    ledc_channel_t ledc_channel;
    ledc_timer_t   ledc_timer;
    gpio_num_t     pin;
    uint8_t        resolution_bits;
    bool           configured;
} pwm_config_t;

static pwm_config_t configs[PWM_CH_COUNT] = {
    [PWM_CH_EMS]       = { .ledc_channel = LEDC_CHANNEL_0, .ledc_timer = LEDC_TIMER_0 },
    [PWM_CH_VIBRATION] = { .ledc_channel = LEDC_CHANNEL_1, .ledc_timer = LEDC_TIMER_1 },
};

void hal_pwm_init(void)
{
    LOG_INFO("PWM HAL initialized (LEDC, %d channels)", PWM_CH_COUNT);
}

void hal_pwm_configure(hal_pwm_channel_t ch, gpio_num_t pin,
                       uint32_t freq_hz, uint8_t resolution_bits)
{
    if (ch >= PWM_CH_COUNT) {
        LOG_ERROR("Invalid PWM channel: %d", ch);
        return;
    }

    configs[ch].pin = pin;
    configs[ch].resolution_bits = resolution_bits;
    configs[ch].configured = true;

    /* 타이머 설정: 주파수와 해상도 결정 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = configs[ch].ledc_timer,
        .duty_resolution = resolution_bits,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        LOG_ERROR("LEDC timer config failed (ch=%d, err=%d)", ch, err);
        return;
    }

    /* 채널 설정: 출력 핀과 타이머 연결 */
    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = configs[ch].ledc_channel,
        .timer_sel  = configs[ch].ledc_timer,
        .gpio_num   = pin,
        .duty       = 0,       /* 초기 듀티비 0 (출력 없음) */
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        LOG_ERROR("LEDC channel config failed (ch=%d, err=%d)", ch, err);
        return;
    }

    LOG_INFO("PWM ch%d configured: GPIO%d, %luHz, %dbit",
             ch, pin, (unsigned long)freq_hz, resolution_bits);
}

void hal_pwm_set_duty(hal_pwm_channel_t ch, uint32_t duty)
{
    if (ch >= PWM_CH_COUNT || !configs[ch].configured) return;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, configs[ch].ledc_channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, configs[ch].ledc_channel);
}

void hal_pwm_set_frequency(hal_pwm_channel_t ch, uint32_t freq_hz)
{
    if (ch >= PWM_CH_COUNT || !configs[ch].configured) return;

    ledc_set_freq(LEDC_LOW_SPEED_MODE, configs[ch].ledc_timer, freq_hz);
    LOG_INFO("PWM ch%d freq changed to %luHz", ch, (unsigned long)freq_hz);
}

void hal_pwm_start(hal_pwm_channel_t ch)
{
    if (ch >= PWM_CH_COUNT || !configs[ch].configured) return;

    /* 현재 설정된 듀티비로 출력 시작 (이미 set_duty로 설정됨) */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, configs[ch].ledc_channel);
}

void hal_pwm_stop(hal_pwm_channel_t ch)
{
    if (ch >= PWM_CH_COUNT || !configs[ch].configured) return;

    /* 듀티비 0으로 설정 → 출력 LOW 고정 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, configs[ch].ledc_channel, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, configs[ch].ledc_channel);
}
