#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

/*
 * OpenGlow 디버그 로그 시스템
 *
 * ESP-IDF의 esp_log를 래핑하여 프로젝트 전용 로그 매크로 제공.
 * 사용법:
 *   LOG_INFO("State transition: %s -> %s", prev_name, next_name);
 *   LOG_WARN("Temperature warning: %.1f°C", temp);
 *
 * 출력 예시:
 *   I (12345) FSM: State transition: IDLE -> MODE_SELECT
 *
 * 로그 레벨은 openglow_config.h의 LOG_LEVEL로 제어.
 */

#include "esp_log.h"
#include "openglow_config.h"

/* 각 모듈의 .c 파일 상단에서 TAG를 정의해야 함:
 * static const char *TAG = "FSM";
 */

#if LOG_LEVEL <= LOG_LEVEL_DEBUG
    #define LOG_DEBUG(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_INFO
    #define LOG_INFO(fmt, ...)  ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#else
    #define LOG_INFO(fmt, ...)  ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_WARN
    #define LOG_WARN(fmt, ...)  ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
    #define LOG_WARN(fmt, ...)  ((void)0)
#endif

/* ERROR는 항상 출력 */
#define LOG_ERROR(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)

#endif /* DEBUG_LOG_H */
