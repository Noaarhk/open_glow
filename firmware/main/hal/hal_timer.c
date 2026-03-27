/*
 * 타이머 및 Watchdog HAL 구현 (ESP32)
 *
 * 시간 조회: esp_timer_get_time() (마이크로초) → 밀리초 변환
 *
 * Watchdog: ESP-IDF의 Task Watchdog Timer (TWDT) 사용.
 *   TWDT는 등록된 태스크가 일정 시간 내에 feed하지 않으면
 *   경고를 출력하거나 시스템을 리셋함.
 *   메인 루프가 있는 main task를 TWDT에 등록하여 감시.
 */

#include "hal_timer.h"
#include "../debug_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HAL_TIMER";

void hal_timer_init(void)
{
    /* esp_timer는 ESP-IDF에서 자동 초기화됨 */
    LOG_INFO("Timer HAL initialized");
}

uint32_t hal_timer_get_ms(void)
{
    /* esp_timer_get_time()은 부팅 후 경과 마이크로초를 반환.
     * 1000으로 나눠 밀리초로 변환.
     * uint32_t로 캐스팅하면 약 49일 후 오버플로 — 뷰티 디바이스에서 충분. */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void hal_watchdog_init(uint32_t timeout_ms)
{
    /* TWDT 설정 구조체 */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = timeout_ms,
        .idle_core_mask = 0,            /* idle 태스크 감시 안 함 */
        .trigger_panic  = true,         /* 타임아웃 시 panic (자동 리셋) */
    };

    /* TWDT 초기화 (이미 초기화된 경우 재설정) */
    esp_err_t err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (err != ESP_OK) {
        err = esp_task_wdt_init(&wdt_cfg);
    }

    if (err != ESP_OK) {
        LOG_ERROR("Watchdog init failed (err=%d)", err);
        return;
    }

    /* 현재 태스크(app_main이 돌아가는 main task)를 TWDT에 등록 */
    err = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    if (err != ESP_OK) {
        LOG_ERROR("Watchdog task add failed (err=%d)", err);
        return;
    }

    LOG_INFO("Watchdog initialized (timeout=%lums)", (unsigned long)timeout_ms);
}

void hal_watchdog_feed(void)
{
    /* 현재 태스크의 watchdog 카운터를 리셋.
     * 메인 루프 시작마다 호출하여 "나 아직 살아있어"를 알림. */
    esp_task_wdt_reset();
}
