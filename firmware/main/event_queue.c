/*
 * OpenGlow 이벤트 큐 구현
 *
 * 링 버퍼 기반 FIFO 큐.
 * critical section으로 ISR/BLE 콜백에서 안전하게 push 가능.
 * 안전 이벤트(EVENT_SAFETY_*)는 오버플로 시에도 보호됨.
 */

#include "event_queue.h"
#include "openglow_config.h"
#include "debug_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "EVTQ";

/* 링 버퍼 */
static event_t queue_buffer[EVENT_QUEUE_SIZE];
static uint8_t head;           /* 다음에 꺼낼 위치 */
static uint8_t tail;           /* 다음에 넣을 위치 */
static uint8_t count;          /* 현재 저장된 이벤트 수 */
static uint32_t overflow_count; /* 오버플로 발생 횟수 (디버그용) */
static portMUX_TYPE queue_mux = portMUX_INITIALIZER_UNLOCKED;

/* 안전 이벤트인지 판별 */
static bool is_safety_event(event_type_t type)
{
    return (type >= EVENT_SAFETY_TEMP_WARNING &&
            type <= EVENT_SAFETY_AUTO_TIMEOUT);
}

void event_queue_init(void)
{
    head = 0;
    tail = 0;
    count = 0;
    overflow_count = 0;
    LOG_INFO("Event queue initialized (size=%d)", EVENT_QUEUE_SIZE);
}

bool event_queue_push(event_t event)
{
    /* 타임스탬프 자동 설정 (호출자가 설정하지 않은 경우) */
    if (event.timestamp_ms == 0) {
        event.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }

    /* critical section 진입 (ISR/BLE 콜백에서 안전) */
    taskENTER_CRITICAL(&queue_mux);

    if (count >= EVENT_QUEUE_SIZE) {
        /* 큐가 가득 찬 경우 */
        if (is_safety_event(event.type)) {
            /*
             * 안전 이벤트 보호: 가장 오래된 비안전 이벤트를 찾아 덮어씀.
             * 안전 이벤트는 절대 드롭하지 않음.
             */
            uint8_t idx = head;
            bool found = false;
            for (uint8_t i = 0; i < count; i++) {
                if (!is_safety_event(queue_buffer[idx].type)) {
                    queue_buffer[idx] = event;
                    found = true;
                    break;
                }
                idx = (idx + 1) % EVENT_QUEUE_SIZE;
            }
            taskEXIT_CRITICAL(&queue_mux);

            if (!found) {
                LOG_ERROR("Queue full with all safety events! Dropping safety event type=%d", event.type);
                return false;
            }
            return true;
        }

        /* 일반 이벤트는 드롭 */
        overflow_count++;
        taskEXIT_CRITICAL(&queue_mux);
        LOG_WARN("Queue overflow (count=%lu), dropped event type=%d",
                 (unsigned long)overflow_count, event.type);
        return false;
    }

    /* 정상 삽입 */
    queue_buffer[tail] = event;
    tail = (tail + 1) % EVENT_QUEUE_SIZE;
    count++;

    taskEXIT_CRITICAL(&queue_mux);
    return true;
}

bool event_queue_pop(event_t *event)
{
    if (event == NULL || count == 0) {
        return false;
    }

    taskENTER_CRITICAL(&queue_mux);

    *event = queue_buffer[head];
    head = (head + 1) % EVENT_QUEUE_SIZE;
    count--;

    taskEXIT_CRITICAL(&queue_mux);
    return true;
}

/* 미사용 — 디버깅/모니터링 필요 시 활성화 예정
bool event_queue_is_empty(void)
{
    return count == 0;
}

uint8_t event_queue_count(void)
{
    return count;
}
*/
