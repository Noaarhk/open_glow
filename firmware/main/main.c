/*
 * OpenGlow 메인 엔트리포인트
 *
 * ESP32가 부팅되면 app_main()이 호출됨.
 * 여기서 모든 모듈을 초기화하고 메인 루프를 실행.
 *
 * 메인 루프 호출 순서:
 *   입력 수집 → 안전 체크 → FSM 판단 → 출력 반영 → BLE 동기화
 *
 * 초기화 순서 (의존관계 기반):
 *   HAL → 이벤트 큐 → 입력 → 출력 → 안전 → FSM → BLE → Watchdog
 */

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "openglow_config.h"
#include "debug_log.h"
#include "event_queue.h"
#include "button_handler.h"
#include "skin_contact.h"
#include "battery_monitor.h"
#include "safety_manager.h"
#include "device_fsm.h"
#include "ems_controller.h"
#include "led_controller.h"
#include "vibration_controller.h"
#include "ble_service.h"
#include "hal/hal_gpio.h"
#include "hal/hal_adc.h"
#include "hal/hal_pwm.h"
#include "hal/hal_timer.h"
#include "hal/hal_nvs.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    LOG_INFO("=================================");
    LOG_INFO("OpenGlow firmware v%d.%d.%d", FW_MAJOR, FW_MINOR, FW_PATCH);
    LOG_INFO("Prototype mode: %s", PROTOTYPE_MODE ? "ON" : "OFF");
    LOG_INFO("=================================");

    /* 모듈별 로그 레벨 개별 설정
     * config.h의 LOG_LEVEL=DEBUG로 전체 DEBUG 컴파일 활성화 후,
     * 런타임에 모듈별로 레벨을 올려서 불필요한 출력을 억제.
     * 디버그 보고 싶은 모듈만 아래 목록에서 제거하면 됨. */
    esp_log_level_set("*", ESP_LOG_INFO);     /* 기본: 전체 INFO */
    esp_log_level_set("BAT", ESP_LOG_DEBUG);  /* 배터리/온도만 DEBUG */

    /* ===== 초기화 순서: 의존관계에 따른 순서 ===== */

    /* 1. HAL 레이어 (하드웨어 접근의 기반, 가장 먼저 초기화) */
    hal_nvs_init();     /* NVS 먼저 — ESP-IDF 내부적으로 다른 컴포넌트가 참조할 수 있음 */
    hal_gpio_init();
    hal_adc_init();
    hal_pwm_init();
    hal_timer_init();

    /* 2. 이벤트 큐 (다른 모듈이 push하므로 모듈 초기화 전에 준비) */
    event_queue_init();

    /* 3. 입력 모듈 */
    button_init();
    skin_contact_init();
    battery_init();

    /* 4. 출력 모듈 */
    ems_init();
    led_init();
    vibration_init();

    /* 5. 안전 관리자 (출력 모듈 이후 — 비상 차단 시 ems_emergency_stop 호출) */
    safety_init();

    /* 6. 상태 머신 (모든 모듈 이후) */
    fsm_init();

    /* 7. BLE 서비스 (FSM 이후 — 앱 명령 수신 준비) */
    ble_init();

    /* 8. Watchdog (모든 초기화 완료 후 마지막에 설정) */
    hal_watchdog_init(WATCHDOG_TIMEOUT_MS);

    LOG_INFO("All modules initialized. Entering main loop.");

    /* ===== 메인 루프: 10ms 주기 (100Hz) ===== */
    while (1) {
        /* Watchdog 리셋 (루프 시작마다 — "나 아직 살아있어") */
        hal_watchdog_feed();

        /* 입력 수집 (센서 → 이벤트 생성) */
        button_update();
        skin_contact_update();
        battery_update();       /* ADC 5초 간격, 충전 GPIO 매 루프 */

        /* 안전 체크 (최신 센서 데이터 기반, battery_update 이후여야 함) */
        safety_update();

        /* 중앙 제어 (이벤트 소비 → 상태 전이) */
        fsm_update();

        /* 안전 출력 제한 반영 (safety → EMS 듀티 스케일링) */
        ems_set_output_limit(safety_get_output_limit());

        /* 출력 업데이트 (FSM이 설정한 값 반영) */
        ems_update();
        led_update();
        vibration_update();

        /* BLE 동기화 (상태 변화 → Notify 전송) */
        ble_update();

        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));
    }
}
