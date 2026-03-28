/*
 * LED 컨트롤러 구현 (WS2812B via ESP-IDF led_strip)
 *
 * 패턴 동작 원리:
 *   led_blink(), led_breathe() 등은 "이 패턴을 시작해라"만 설정.
 *   실제 on/off 토글, 밝기 변화는 led_update()에서 매 10ms마다 처리.
 *   Django의 Celery beat처럼, 주기적으로 실행되면서 상태를 갱신하는 방식.
 *
 * 밝기 스케일링:
 *   LED에 별도 밝기 레지스터가 없으므로, RGB 값 자체를 줄여서 밝기 조절.
 *   (R, G, B) × (brightness / 255) = 실제 출력 색상
 */

#include "led_controller.h"
#include "hal/hal_timer.h"
#include "debug_log.h"
#include "led_strip.h"

static const char *TAG = "LED";

/* LED 패턴 종류 */
typedef enum {
    LED_PATTERN_OFF,
    LED_PATTERN_SOLID,      /* 단색 고정 */
    LED_PATTERN_BLINK,      /* 깜빡임 */
    LED_PATTERN_BREATHE,    /* 숨쉬기 */
    LED_PATTERN_FADE_OUT,   /* 페이드아웃 */
} led_pattern_t;

/* 모드별 기본 색상 테이블 */
static const uint8_t mode_colors[MODE_COUNT][3] = {
    [MODE_PULSE] = { 255, 100,   0 },  /* 주황 */
    [MODE_MICRO]      = {   0, 200, 255 },  /* 하늘 */
    [MODE_EMS]     = {   0, 255, 100 },  /* 초록 */
    [MODE_THERMAL] = { 200,   0, 255 },  /* 보라 */
};

/* 내부 상태 */
static struct {
    led_strip_handle_t strip;       /* led_strip 드라이버 핸들 */

    /* 현재 설정된 색상 (밝기 적용 전 원본) */
    uint8_t base_r, base_g, base_b;
    uint8_t brightness;             /* 0~255 */

    /* 패턴 상태 */
    led_pattern_t pattern;
    uint16_t blink_on_ms;
    uint16_t blink_off_ms;
    bool blink_is_on;
    uint32_t blink_toggle_time_ms;

    uint16_t breathe_period_ms;
    uint32_t breathe_start_ms;

    uint16_t fade_duration_ms;
    uint32_t fade_start_ms;
    uint8_t fade_start_brightness;
} ctx;

/* strip에 현재 색상 반영 (밝기 스케일링 적용) */
static void apply_color(void)
{
    uint8_t r = (uint8_t)((uint16_t)ctx.base_r * ctx.brightness / 255);
    uint8_t g = (uint8_t)((uint16_t)ctx.base_g * ctx.brightness / 255);
    uint8_t b = (uint8_t)((uint16_t)ctx.base_b * ctx.brightness / 255);

    led_strip_set_pixel(ctx.strip, 0, r, g, b);
    led_strip_refresh(ctx.strip);
}

void led_init(void)
{
    /* led_strip 드라이버 설정 (RMT 백엔드) */
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED_DATA,
        .max_leds       = 1,                  /* WS2812B 1개 */
        .led_model      = LED_MODEL_WS2812,
        .color_component_format = {
            .format = {
                .r_pos = 1,   /* GRB 순서 (WS2812B 표준) */
                .g_pos = 0,
                .b_pos = 2,
                .num_components = 3,
            },
        },
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src    = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,            /* 10MHz → 100ns 정밀도 */
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &ctx.strip);
    if (err != ESP_OK) {
        LOG_ERROR("LED strip init failed (err=%d)", err);
        return;
    }

    ctx.brightness = 255;
    ctx.pattern = LED_PATTERN_OFF;
    led_strip_clear(ctx.strip);

    LOG_INFO("LED controller initialized (GPIO%d, WS2812B)", PIN_LED_DATA);
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ctx.base_r = r;
    ctx.base_g = g;
    ctx.base_b = b;

    if (ctx.pattern == LED_PATTERN_SOLID) {
        apply_color();
    }
}

void led_set_mode_color(device_mode_t mode)
{
    if (mode >= MODE_COUNT) return;
    ctx.base_r = mode_colors[mode][0];
    ctx.base_g = mode_colors[mode][1];
    ctx.base_b = mode_colors[mode][2];

    /* 모드 색상 설정은 SOLID 패턴으로 전환 */
    ctx.pattern = LED_PATTERN_SOLID;
    apply_color();
}

void led_set_brightness(uint8_t brightness)
{
    ctx.brightness = brightness;
    if (ctx.pattern == LED_PATTERN_SOLID) {
        apply_color();
    }
}

void led_blink(uint16_t on_ms, uint16_t off_ms)
{
    ctx.pattern = LED_PATTERN_BLINK;
    ctx.blink_on_ms = on_ms;
    ctx.blink_off_ms = off_ms;
    ctx.blink_is_on = true;
    ctx.blink_toggle_time_ms = hal_timer_get_ms() + on_ms;
    apply_color();
}

void led_breathe(uint16_t period_ms)
{
    ctx.pattern = LED_PATTERN_BREATHE;
    ctx.breathe_period_ms = period_ms;
    ctx.breathe_start_ms = hal_timer_get_ms();
}

void led_fade_out(uint16_t duration_ms)
{
    ctx.pattern = LED_PATTERN_FADE_OUT;
    ctx.fade_duration_ms = duration_ms;
    ctx.fade_start_ms = hal_timer_get_ms();
    ctx.fade_start_brightness = ctx.brightness;
}

void led_show_battery_level(uint8_t percent)
{
    /* 배터리 잔량에 따른 색상 그라데이션:
     *   0~30%  → 빨강 (255, 0, 0)
     *  30~70%  → 주황 (255, 165, 0)
     *  70~100% → 초록 (0, 255, 0)  */
    if (percent <= 30) {
        ctx.base_r = 255; ctx.base_g = 0; ctx.base_b = 0;
    } else if (percent <= 70) {
        ctx.base_r = 255; ctx.base_g = 165; ctx.base_b = 0;
    } else {
        ctx.base_r = 0; ctx.base_g = 255; ctx.base_b = 0;
    }
    ctx.brightness = 255;

    /* 충전 중에는 숨쉬기 패턴 */
    led_breathe(LED_BREATHE_PERIOD_MS);
}

void led_update(void)
{
    uint32_t now = hal_timer_get_ms();

    switch (ctx.pattern) {
        case LED_PATTERN_BLINK:
            if (now >= ctx.blink_toggle_time_ms) {
                ctx.blink_is_on = !ctx.blink_is_on;
                ctx.blink_toggle_time_ms = now +
                    (ctx.blink_is_on ? ctx.blink_on_ms : ctx.blink_off_ms);

                if (ctx.blink_is_on) {
                    apply_color();
                } else {
                    led_strip_clear(ctx.strip);
                }
            }
            break;

        case LED_PATTERN_BREATHE: {
            /* 삼각파: 0→255→0을 period_ms 동안 반복 */
            uint32_t elapsed = (now - ctx.breathe_start_ms) % ctx.breathe_period_ms;
            uint32_t half = ctx.breathe_period_ms / 2;
            uint8_t br;
            if (elapsed < half) {
                /* 상승 구간: 0 → 255 */
                br = (uint8_t)(255 * elapsed / half);
            } else {
                /* 하강 구간: 255 → 0 */
                br = (uint8_t)(255 * (ctx.breathe_period_ms - elapsed) / half);
            }
            ctx.brightness = br;
            apply_color();
            break;
        }

        case LED_PATTERN_FADE_OUT: {
            uint32_t elapsed = now - ctx.fade_start_ms;
            if (elapsed >= ctx.fade_duration_ms) {
                /* 페이드 완료 → 소등 */
                ctx.brightness = 0;
                ctx.pattern = LED_PATTERN_OFF;
                led_strip_clear(ctx.strip);
            } else {
                /* 선형 감소 */
                ctx.brightness = ctx.fade_start_brightness -
                    (uint8_t)(ctx.fade_start_brightness * elapsed / ctx.fade_duration_ms);
                apply_color();
            }
            break;
        }

        case LED_PATTERN_SOLID:
        case LED_PATTERN_OFF:
        default:
            /* 업데이트 불필요 */
            break;
    }
}

void led_off(void)
{
    ctx.pattern = LED_PATTERN_OFF;
    ctx.brightness = 0;
    led_strip_clear(ctx.strip);
}
