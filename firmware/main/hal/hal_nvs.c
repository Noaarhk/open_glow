/*
 * NVS HAL 구현 (ESP32)
 *
 * ESP-IDF의 nvs_flash 컴포넌트를 래핑.
 *
 * NVS 동작 원리:
 *   ESP32 플래시 메모리의 전용 파티션에 key-value 쌍을 저장.
 *   wear leveling(마모 균등화)이 내장되어 플래시 수명을 보호.
 *   네임스페이스로 모듈별 데이터를 분리 가능.
 *
 * 이 구현에서는 "openglow" 네임스페이스 하나만 사용.
 */

#include "hal_nvs.h"
#include "../debug_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "HAL_NVS";

#define NVS_NAMESPACE "openglow"

void hal_nvs_init(void)
{
    /* NVS 플래시 파티션 초기화.
     * ESP_ERR_NVS_NO_FREE_PAGES: 파티션이 꽉 찬 경우
     * ESP_ERR_NVS_NEW_VERSION_FOUND: 이전 버전 데이터가 있는 경우
     * → 두 경우 모두 파티션을 지우고 다시 초기화 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        LOG_WARN("NVS partition issue, erasing and re-init");
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        LOG_ERROR("NVS init failed (err=%d)", err);
        return;
    }

    LOG_INFO("NVS HAL initialized (namespace='%s')", NVS_NAMESPACE);
}

bool hal_nvs_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_ERROR("NVS open failed for set_u32 (err=%d)", err);
        return false;
    }

    err = nvs_set_u32(handle, key, value);
    if (err != ESP_OK) {
        LOG_ERROR("NVS set_u32 failed: key='%s' (err=%d)", key, err);
        nvs_close(handle);
        return false;
    }

    /* commit: 실제로 플래시에 기록 (Django의 db.commit()과 유사) */
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        LOG_ERROR("NVS commit failed (err=%d)", err);
        return false;
    }
    return true;
}

bool hal_nvs_get_u32(const char *key, uint32_t *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;  /* 네임스페이스 없으면 데이터도 없음 */
    }

    err = nvs_get_u32(handle, key, value);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;  /* 키가 없음 (에러 아님, 첫 실행 시 정상) */
    }
    if (err != ESP_OK) {
        LOG_ERROR("NVS get_u32 failed: key='%s' (err=%d)", key, err);
        return false;
    }
    return true;
}

bool hal_nvs_set_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_ERROR("NVS open failed for set_blob (err=%d)", err);
        return false;
    }

    err = nvs_set_blob(handle, key, data, len);
    if (err != ESP_OK) {
        LOG_ERROR("NVS set_blob failed: key='%s', len=%u (err=%d)",
                  key, (unsigned)len, err);
        nvs_close(handle);
        return false;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        LOG_ERROR("NVS commit failed (err=%d)", err);
        return false;
    }
    return true;
}

bool hal_nvs_get_blob(const char *key, void *data, size_t *len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_get_blob(handle, key, data, len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        LOG_ERROR("NVS get_blob failed: key='%s' (err=%d)", key, err);
        return false;
    }
    return true;
}
