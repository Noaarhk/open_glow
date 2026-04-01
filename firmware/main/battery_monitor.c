/*
 * 배터리/온도 모니터 구현
 *
 * ADC 파이프라인:
 *   1. ADC raw 읽기 (0~4095, 12bit)
 *   2. 유효성 검사 (0 또는 4095면 센서 고장 의심)
 *   3. 이동평균 필터 (최근 16샘플 평균)
 *   4. 전압 변환: voltage = raw × 3.3 / 4095 × 분압비
 *   5. 퍼센트 변환: 선형 보간 (4.2V=100%, 3.0V=0%)
 *
 * NTC 온도 변환 (Steinhart-Hart 간이식):
 *   1/T = 1/T0 + (1/B) × ln(R/R0)
 *   T0=298.15K(25°C), B=3950, R0=10kΩ
 *
 * 충전 감지:
 *   충전 IC STAT 핀(GPIO27): LOW=충전 중, HIGH=비충전/완료
 *   매 루프마다 GPIO 읽기 → 상태 변화 시 이벤트 push
 */

#include "battery_monitor.h"
#include "openglow_config.h"
#include "event_queue.h"
#include "hal/hal_adc.h"
#include "hal/hal_gpio.h"
#include "hal/hal_timer.h"
#include "debug_log.h"
#include <math.h>

static const char *TAG = "BAT";

/* 분압 비율: 실제 배터리 전압 = ADC 전압 × 이 값
 * 전형적인 1:1 분압기(10kΩ+10kΩ)이면 2.0
 * TODO: 실제 분압 회로에 맞게 조정 */
#define VOLTAGE_DIVIDER_RATIO   2.0f
#define ADC_VREF                3.3f
#define ADC_MAX_RAW             4095

/* 이동평균 필터 */
typedef struct {
    uint16_t samples[BATTERY_FILTER_SIZE];
    uint8_t index;
    uint8_t count;      /* 필터가 채워진 샘플 수 (초기 구간 처리) */
    uint32_t sum;
} moving_avg_t;

static struct {
    moving_avg_t voltage_filter;
    moving_avg_t temp_filter;

    float voltage;              /* 필터링된 배터리 전압 (V) */
    float temperature;          /* 필터링된 온도 (°C) */
    uint8_t percent;            /* 배터리 잔량 (%) */

    bool charging;              /* 현재 충전 상태 */
    bool prev_charging;         /* 이전 충전 상태 (변화 감지용) */

    bool valid;                 /* ADC 읽기 유효성 */
    uint32_t last_adc_ms;       /* 마지막 ADC 읽기 시각 */
} ctx;

/* === 이동평균 필터 === */

static void filter_init(moving_avg_t *f)
{
    f->index = 0;
    f->count = 0;
    f->sum = 0;
    for (int i = 0; i < BATTERY_FILTER_SIZE; i++) {
        f->samples[i] = 0;
    }
}

static uint16_t filter_add(moving_avg_t *f, uint16_t sample)
{
    /* 오래된 샘플 제거 */
    f->sum -= f->samples[f->index];
    /* 새 샘플 추가 */
    f->samples[f->index] = sample;
    f->sum += sample;
    f->index = (f->index + 1) % BATTERY_FILTER_SIZE;

    if (f->count < BATTERY_FILTER_SIZE) {
        f->count++;
    }

    return (uint16_t)(f->sum / f->count);
}

/* === 변환 함수 === */

static float adc_to_voltage(uint16_t filtered_raw)
{
    float adc_voltage = (float)filtered_raw * ADC_VREF / ADC_MAX_RAW;
    return adc_voltage * VOLTAGE_DIVIDER_RATIO;
}

static uint8_t voltage_to_percent(float voltage)
{
    /* 선형 보간: 3.0V=0%, 4.2V=100% */
    if (voltage >= BATTERY_FULL_VOLTAGE) return 100;
    if (voltage <= BATTERY_EMPTY_VOLTAGE) return 0;

    float ratio = (voltage - BATTERY_EMPTY_VOLTAGE) /
                  (BATTERY_FULL_VOLTAGE - BATTERY_EMPTY_VOLTAGE);
    return (uint8_t)(ratio * 100.0f);
}

static float adc_to_temperature(uint16_t filtered_raw)
{
    /* NTC 저항 계산: 전압 분배 공식 역산
     * V_adc = Vref × R_ntc / (R_series + R_ntc)
     * → R_ntc = R_series / (ADC_MAX / raw - 1) */
    if (filtered_raw == 0) return -999.0f;  /* 0으로 나누기 방지 */

    float resistance = (float)NTC_SERIES_RESISTOR /
                       ((float)ADC_MAX_RAW / (float)filtered_raw - 1.0f);

    /* Steinhart-Hart 간이식 (B-parameter equation) */
    float steinhart = logf(resistance / (float)NTC_NOMINAL_R) / (float)NTC_B_COEFFICIENT;
    steinhart += 1.0f / (NTC_NOMINAL_TEMP + 273.15f);

    return (1.0f / steinhart) - 273.15f;
}

/* === 공개 함수 === */

void battery_init(void)
{
    filter_init(&ctx.voltage_filter);
    filter_init(&ctx.temp_filter);

    /* 충전 IC STAT 핀: 입력, 풀업 활성화 (오픈드레인 출력 대응) */
    hal_gpio_set_input(PIN_CHARGE_STAT, true);

    ctx.voltage = BATTERY_FULL_VOLTAGE;
    ctx.temperature = 25.0f;
    ctx.percent = 100;
    ctx.charging = false;
    ctx.prev_charging = false;
    ctx.valid = true;
    ctx.last_adc_ms = 0;

    LOG_INFO("Battery monitor initialized (V=GPIO34, T=GPIO35, STAT=GPIO%d)",
             PIN_CHARGE_STAT);
}

void battery_update(void)
{
    uint32_t now = hal_timer_get_ms();

    /* --- 충전 상태 GPIO: 매 루프마다 체크 (즉시 반응 필요) --- */
    /* STAT 핀: LOW = 충전 중, HIGH = 비충전/완료 (일반적인 충전 IC 동작) */
    ctx.charging = (hal_gpio_read(PIN_CHARGE_STAT) == 0);

    if (ctx.charging != ctx.prev_charging) {
        event_t evt = {
            .data = 0,
            .timestamp_ms = now,
        };

        if (ctx.charging) {
            evt.type = EVENT_CHARGE_CONNECTED;
            LOG_INFO("Charger connected");
        } else if (ctx.percent >= 95) {
            /* 충전 중 → 비충전 전환 + 배터리 거의 만충 → 충전 완료 */
            evt.type = EVENT_CHARGE_COMPLETE;
            LOG_INFO("Charge complete (%d%%)", ctx.percent);
        } else {
            evt.type = EVENT_CHARGE_DISCONNECTED;
            LOG_INFO("Charger disconnected");
        }

        event_queue_push(evt);
        ctx.prev_charging = ctx.charging;
    }

    /* --- ADC 읽기: 5초 간격 --- */
    if (now - ctx.last_adc_ms < BATTERY_UPDATE_INTERVAL_MS) return;
    ctx.last_adc_ms = now;

    /* 배터리 전압 ADC */
    uint16_t v_raw = hal_adc_read(PIN_BATTERY_ADC);

    /* 온도 ADC */
    uint16_t t_raw = hal_adc_read(PIN_TEMP_ADC);

    /* 유효성 검사: raw가 0 또는 4095이면 센서 고장 의심 */
    if ((v_raw == 0 || v_raw == ADC_MAX_RAW) &&
        (t_raw == 0 || t_raw == ADC_MAX_RAW)) {
        ctx.valid = false;
        LOG_WARN("ADC sensor fault suspected (V_raw=%d, T_raw=%d)", v_raw, t_raw);
        return;
    }
    ctx.valid = true;

    /* 이동평균 필터 적용 */
    uint16_t v_filtered = filter_add(&ctx.voltage_filter, v_raw);
    uint16_t t_filtered = filter_add(&ctx.temp_filter, t_raw);

    /* 변환 */
    ctx.voltage = adc_to_voltage(v_filtered);
    ctx.percent = voltage_to_percent(ctx.voltage);
    ctx.temperature = adc_to_temperature(t_filtered);

    LOG_DEBUG("BAT: %.2fV (%d%%), TEMP: %.1f°C",
              ctx.voltage, ctx.percent, ctx.temperature);
}

uint8_t battery_get_percent(void)
{
    return ctx.percent;
}

float battery_get_voltage(void)
{
    return ctx.voltage;
}

float battery_get_temperature(void)
{
    return ctx.temperature;
}

bool battery_is_charging(void)
{
    return ctx.charging;
}

bool battery_is_valid(void)
{
    return ctx.valid;
}
