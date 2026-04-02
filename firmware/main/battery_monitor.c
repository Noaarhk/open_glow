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
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
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

    bool voltage_connected;     /* 초기화 시 감지된 연결 상태 (부팅 후 고정) */
    bool temp_connected;        /* 초기화 시 감지된 연결 상태 (부팅 후 고정) */
    bool voltage_valid;         /* 런타임 유효성 (매 읽기마다 갱신) */
    bool temp_valid;            /* 런타임 유효성 (매 읽기마다 갱신) */
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

/* === 초기화 시 센서 연결 감지 === */

static bool detect_sensor_voltage(void)
{
    int valid_count = 0;
    for (int i = 0; i < SENSOR_DETECT_SAMPLES; i++) {
        uint16_t raw = hal_adc_read(PIN_BATTERY_ADC);
        if (raw > 0 && raw < ADC_MAX_RAW) {
            float v = adc_to_voltage(raw);
            if (v > 2.5f && v < 4.5f) valid_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (valid_count >= SENSOR_DETECT_THRESHOLD);
}

static bool detect_sensor_temp(void)
{
    int valid_count = 0;
    for (int i = 0; i < SENSOR_DETECT_SAMPLES; i++) {
        uint16_t raw = hal_adc_read(PIN_TEMP_ADC);
        if (raw > 0 && raw < ADC_MAX_RAW) {
            float t = adc_to_temperature(raw);
            if (t > -20.0f && t < 80.0f) valid_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (valid_count >= SENSOR_DETECT_THRESHOLD);
}

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
    ctx.voltage_valid = false;
    ctx.temp_valid = false;
    ctx.last_adc_ms = 0;

    /* 센서 연결 감지: ADC를 여러 번 읽어 범위 안이면 "연결됨"
     * 감지 성공 시 valid도 true로 세팅 — safety_manager가 첫 ADC 읽기 전에
     * "connected인데 valid 아님 → 고장"으로 오판하는 것을 방지 */
    ctx.voltage_connected = detect_sensor_voltage();
    ctx.voltage_valid = ctx.voltage_connected;
    ctx.temp_connected = detect_sensor_temp();
    ctx.temp_valid = ctx.temp_connected;

    LOG_INFO("Battery monitor initialized (V=GPIO34 [%s], T=GPIO35 [%s], STAT=GPIO%d)",
             ctx.voltage_connected ? "CONNECTED" : "NOT DETECTED",
             ctx.temp_connected ? "CONNECTED" : "NOT DETECTED",
             PIN_CHARGE_STAT);

    if (!ctx.voltage_connected) {
        LOG_WARN("Battery voltage sensor not detected — voltage safety will use fallback policy");
    }
    if (!ctx.temp_connected) {
        LOG_WARN("NTC temp sensor not detected — temperature safety will use fallback policy");
    }
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

    /* 배터리 전압 ADC — raw 체크 + 전압 범위 체크 (2.5~4.5V 밖이면 미연결) */
    uint16_t v_raw = hal_adc_read(PIN_BATTERY_ADC);
    if (v_raw > 0 && v_raw < ADC_MAX_RAW) {
        uint16_t v_filtered = filter_add(&ctx.voltage_filter, v_raw);
        float voltage = adc_to_voltage(v_filtered);
        if (voltage > 2.5f && voltage < 4.5f) {
            ctx.voltage = voltage;
            ctx.percent = voltage_to_percent(ctx.voltage);
            ctx.voltage_valid = true;
        } else {
            if (ctx.voltage_valid) LOG_WARN("Battery voltage out of range (%.2fV), sensor disconnected?", voltage);
            ctx.voltage_valid = false;
        }
    } else {
        if (ctx.voltage_valid) LOG_WARN("Battery ADC fault (raw=%d)", v_raw);
        ctx.voltage_valid = false;
    }

    /* 온도 ADC — 변환 후 범위 체크 (-20~80°C 밖이면 센서 미연결/고장) */
    uint16_t t_raw = hal_adc_read(PIN_TEMP_ADC);
    if (t_raw > 0 && t_raw < ADC_MAX_RAW) {
        uint16_t t_filtered = filter_add(&ctx.temp_filter, t_raw);
        float temp = adc_to_temperature(t_filtered);
        if (temp > -20.0f && temp < 80.0f) {
            ctx.temperature = temp;
            ctx.temp_valid = true;
        } else {
            if (ctx.temp_valid) LOG_WARN("Temp out of range (%.1f°C), sensor disconnected?", temp);
            ctx.temp_valid = false;
        }
    } else {
        if (ctx.temp_valid) LOG_WARN("Temp ADC fault (raw=%d)", t_raw);
        ctx.temp_valid = false;
    }

    LOG_DEBUG("BAT: %.2fV (%d%%) [%s], TEMP: %.1f°C [%s]",
              ctx.voltage, ctx.percent, ctx.voltage_valid ? "OK" : "N/A",
              ctx.temperature, ctx.temp_valid ? "OK" : "N/A");
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

bool battery_is_voltage_connected(void)
{
    return ctx.voltage_connected;
}

bool battery_is_temp_connected(void)
{
    return ctx.temp_connected;
}

bool battery_is_voltage_valid(void)
{
    return ctx.voltage_valid;
}

bool battery_is_temp_valid(void)
{
    return ctx.temp_valid;
}
