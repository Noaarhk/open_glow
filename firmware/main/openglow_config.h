#ifndef OPENGLOW_CONFIG_H
#define OPENGLOW_CONFIG_H

/*
 * OpenGlow 전체 설정값 정의
 * 모든 모듈이 참조하는 중앙 설정 파일
 */

#include "driver/gpio.h"
#include "driver/adc.h"

/* ===== 펌웨어 버전 ===== */
#define FW_MAJOR 0
#define FW_MINOR 1
#define FW_PATCH 0

/* ===== 빌드 모드 ===== */
#define PROTOTYPE_MODE 1    /* 1: 프로토타입 (EMS→LED 시각화), 0: 양산 */

/* ===== 디바이스 모드 ===== */
typedef enum {
    MODE_PULSE = 0,
    MODE_MICRO,             /* 마이크로커런트 */
    MODE_EMS,               /* 근육 자극 */
    MODE_THERMAL,
    MODE_COUNT
} device_mode_t;

/* ===== FSM 상태 ===== */
typedef enum {
    STATE_OFF = 0,
    STATE_IDLE,
    STATE_MODE_SELECT,
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_CHARGING,
    STATE_ERROR,
    STATE_SHUTDOWN,
    STATE_COUNT
} device_state_t;

/* ===== 에러 코드 ===== */
typedef enum {
    ERR_NONE              = 0x00,
    ERR_TEMP_WARNING      = 0x01,   /* 복구 가능 */
    ERR_TEMP_CRITICAL     = 0x02,   /* 복구 불가 → SHUTDOWN */
    ERR_BATTERY_LOW       = 0x10,   /* 복구 가능 (충전 시) */
    ERR_BATTERY_CRITICAL  = 0x11,   /* 복구 불가 → SHUTDOWN */
    ERR_EMS_FAULT         = 0x20,   /* 출력 이상 감지 */
    ERR_SENSOR_FAULT      = 0x30,   /* ADC 읽기 실패 */
    ERR_WATCHDOG_RESET    = 0x40    /* watchdog에 의한 리셋 */
} error_code_t;

/* ===== 핀 매핑 (ESP32 DevKitC V4) ===== */
#define PIN_BTN_POWER           GPIO_NUM_0      /* ⚠️ 부트 핀, 외부 풀업 필수 */
#define PIN_BTN_MODE            GPIO_NUM_4
#define PIN_EMS_PWM             GPIO_NUM_25
#define PIN_EMS_ENABLE          GPIO_NUM_26     /* 비상 차단용 별도 핀 */
#define PIN_LED_DATA            GPIO_NUM_18     /* WS2812B RMT */
#define PIN_VIBRATION_PWM       GPIO_NUM_19
#define PIN_CHARGE_STAT         GPIO_NUM_27     /* 충전 IC STAT 핀 */
#define PIN_SKIN_CONTACT        GPIO_NUM_32     /* TTP223 터치 센서 (디지털 출력) */
#define PIN_BATTERY_ADC         ADC1_CHANNEL_6  /* GPIO34 */
#define PIN_TEMP_ADC            ADC1_CHANNEL_7  /* GPIO35 */

/* ===== 버튼 ===== */
#define BTN_DEBOUNCE_MS         50
#define BTN_SHORT_MAX_MS        2000
#define BTN_LONG_MS             2000
#define BTN_VLONG_MS            3000

/* ===== FSM 타이머 ===== */
#define IDLE_AUTO_SLEEP_MS      180000  /* 3분 */
#define MODE_SELECT_TIMEOUT_MS  10000   /* 10초 */
#define PAUSE_TIMEOUT_MS        10000   /* 10초 */
#define PAUSE_RESUME_MS         3000    /* 3초 (피부 재접촉) */
#define RUNNING_MAX_MS          600000  /* 10분 */
#define RUNNING_WARNING_MS      480000  /* 8분 (경고) */

/* ===== EMS PWM ===== */
#define EMS_PULSE_FREQ_HZ       1000
#define EMS_MICRO_FREQ_HZ       10      /* 소프트웨어 토글 */
#define EMS_EMS_FREQ_HZ         3000
#define EMS_THERMAL_FREQ_HZ     5000
#define EMS_PWM_RESOLUTION_BITS 13      /* 8192 단계 */

/* ===== PID ===== */
#define PID_KP                  2.0f
#define PID_KI                  0.5f
#define PID_KD                  0.1f
#define PID_INTEGRAL_LIMIT      100.0f  /* anti-windup */
#define PID_TRANSITION_TIME_MS  300     /* 목표 전환 시간 (ms) */

/* ===== 안전 ===== */
#define SAFETY_TEMP_WARNING_C       40.0f
#define SAFETY_TEMP_LIMIT_50_C      45.0f
#define SAFETY_TEMP_CRITICAL_C      50.0f
#define SAFETY_TEMP_RECOVER_C       38.0f   /* 히스테리시스 */
#define SAFETY_TEMP_RECOVER_DELAY_MS 5000
#define SAFETY_BATTERY_LOW_PCT      10
#define SAFETY_BATTERY_CRIT_PCT     5
#define SAFETY_SHOT_COOLDOWN_MS     500

/* ===== 배터리 ===== */
#define BATTERY_UPDATE_INTERVAL_MS  5000
#define BATTERY_FILTER_SIZE         16
#define BATTERY_FULL_VOLTAGE        4.2f
#define BATTERY_EMPTY_VOLTAGE       3.0f
#define NTC_B_COEFFICIENT           3950
#define NTC_NOMINAL_R               10000   /* 10kΩ */
#define NTC_NOMINAL_TEMP            25.0f   /* °C */
#define NTC_SERIES_RESISTOR         10000   /* 직렬 저항 10kΩ */

/* ===== 피부 접촉 감지 (TTP223 터치 센서) ===== */
#define SKIN_CONTACT_DEBOUNCE_MS    100     /* GPIO 상태 변경 디바운스 */
#define SKIN_CONTACT_ACTIVE_LEVEL   1       /* TTP223: HIGH=접촉, LOW=비접촉 */

/* ===== 진동 ===== */
#define VIBRATION_MIN_DUTY_PCT      30
#define VIBRATION_PWM_FREQ_HZ       20000   /* 가청 범위 밖 */
#define VIBRATION_PWM_RESOLUTION_BITS 10    /* 1024 단계 (20kHz에서 13bit 불가: 80MHz/20kHz=4000 < 8192) */

/* ===== LED ===== */
#define LED_BREATHE_PERIOD_MS   2000
#define LED_BLINK_FAST_MS       200
#define LED_BLINK_SLOW_MS       500
#define LED_FADE_OUT_MS         1000

/* ===== BLE ===== */
#define BLE_DEVICE_NAME         "OpenGlow"
#define BLE_MAX_CONNECTIONS     1

/* ===== 이벤트 큐 ===== */
#define EVENT_QUEUE_SIZE        16

/* ===== Watchdog ===== */
#define WATCHDOG_TIMEOUT_MS     5000

/* ===== 메인 루프 ===== */
#define MAIN_LOOP_PERIOD_MS     10      /* 100Hz */

/* ===== 디버그 로그 레벨 ===== */
#define LOG_LEVEL_DEBUG  0
#define LOG_LEVEL_INFO   1
#define LOG_LEVEL_WARN   2
#define LOG_LEVEL_ERROR  3

#define LOG_LEVEL LOG_LEVEL_INFO

#endif /* OPENGLOW_CONFIG_H */
