#ifndef HAL_GPIO_H
#define HAL_GPIO_H

/*
 * GPIO 하드웨어 추상화 레이어
 *
 * ESP-IDF의 GPIO 드라이버를 래핑하여 모듈에서 직접 ESP-IDF를 호출하지 않도록 함.
 * 나중에 PC mock으로 교체하면 실제 하드웨어 없이 단위 테스트 가능.
 *
 * 사용 예:
 *   hal_gpio_set_input(PIN_SKIN_CONTACT, false);  // 입력, 풀업 없음
 *   int level = hal_gpio_read(PIN_SKIN_CONTACT);   // 0 또는 1
 *
 *   hal_gpio_set_output(PIN_EMS_ENABLE);
 *   hal_gpio_write(PIN_EMS_ENABLE, 0);  // LOW = 차단
 */

#include "driver/gpio.h"

/* GPIO 서브시스템 초기화 (app_main에서 가장 먼저 호출) */
void hal_gpio_init(void);

/* 핀을 출력 모드로 설정 (초기값 LOW) */
void hal_gpio_set_output(gpio_num_t pin);

/* 핀을 입력 모드로 설정 (pull_up=true이면 내부 풀업 활성화) */
void hal_gpio_set_input(gpio_num_t pin, bool pull_up);

/* 핀 레벨 읽기 (0=LOW, 1=HIGH) */
int hal_gpio_read(gpio_num_t pin);

/* 핀 레벨 쓰기 (0=LOW, 1=HIGH) */
void hal_gpio_write(gpio_num_t pin, int level);

#endif /* HAL_GPIO_H */
