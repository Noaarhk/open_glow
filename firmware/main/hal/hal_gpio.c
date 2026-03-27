/*
 * GPIO HAL 구현 (ESP32)
 *
 * ESP-IDF의 gpio 드라이버를 래핑.
 * hal_gpio_init()은 공통 초기화만 수행하고,
 * 개별 핀 설정은 hal_gpio_set_input/output()에서 수행.
 */

#include "hal_gpio.h"
#include "../debug_log.h"

static const char *TAG = "HAL_GPIO";

void hal_gpio_init(void)
{
    /* GPIO 서브시스템은 ESP-IDF에서 자동 초기화됨.
     * 여기서는 로그만 출력. 개별 핀 설정은 각 모듈의 init()에서 수행. */
    LOG_INFO("GPIO HAL initialized");
}

void hal_gpio_set_output(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);  /* 초기값 LOW (안전) */
}

void hal_gpio_set_input(gpio_num_t pin, bool pull_up)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

int hal_gpio_read(gpio_num_t pin)
{
    return gpio_get_level(pin);
}

void hal_gpio_write(gpio_num_t pin, int level)
{
    gpio_set_level(pin, level);
}
