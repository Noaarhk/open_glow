/*
 * BLE GATT 서버 구현
 *
 * ESP-IDF Bluedroid 스택 기반.
 * 커스텀 서비스 1개 + Characteristic 9개.
 *
 * Write 콜백 → event_queue push (FSM 직접 호출 금지).
 * ble_update() → 상태 변화 감지 → Notify 전송.
 *
 * Characteristic 목록:
 *   0: Device Info     (Read)              3 bytes  - FW 버전
 *   1: Device State    (Read + Notify)     1 byte   - FSM 상태
 *   2: Mode            (Read/Write/Notify) 1 byte   - 모드
 *   3: Intensity       (Read/Write/Notify) 1 byte   - 세기
 *   4: LED Color       (Read/Write)        3 bytes  - RGB
 *   5: Battery         (Read + Notify)     1 byte   - 배터리 %
 *   6: Temperature     (Read + Notify)     1 byte   - 온도 (정수)
 *   7: Safety Status   (Read + Notify)     2 bytes  - 안전 상태
 *   8: Error Log       (Read)              max 10   - 에러 코드 배열
 *   9: Session Log     (Read + Notify)     18 bytes - 세션 로그 (packed struct)
 */

#include "ble_service.h"
#include "openglow_config.h"
#include "event_queue.h"
#include "device_fsm.h"
#include "battery_monitor.h"
#include "safety_manager.h"
#include "session_log.h"
#include "led_controller.h"
#include "debug_log.h"
#include "hal/hal_timer.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"

#include <string.h>

static const char *TAG = "BLE";

/* ===== UUID 정의 ===== */
/* 128-bit 커스텀 서비스 UUID: 0000FFE0-0000-1000-8000-00805F9B34FB */
static const uint8_t SERVICE_UUID[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE0, 0xFF, 0x00, 0x00
};

/* Characteristic UUID: 0000FFE1 ~ 0000FFE9 */
#define CHAR_UUID_BASE          0xFFE1
#define CHAR_COUNT              10

/* Characteristic 인덱스 */
enum {
    CHAR_DEVICE_INFO = 0,   /* 0xFFE1 */
    CHAR_DEVICE_STATE,      /* 0xFFE2 */
    CHAR_MODE,              /* 0xFFE3 */
    CHAR_INTENSITY,         /* 0xFFE4 */
    CHAR_LED_COLOR,         /* 0xFFE5 */
    CHAR_BATTERY,           /* 0xFFE6 */
    CHAR_TEMPERATURE,       /* 0xFFE7 */
    CHAR_SAFETY_STATUS,     /* 0xFFE8 */
    CHAR_ERROR_LOG,         /* 0xFFE9 */
    CHAR_SESSION_LOG,       /* 0xFFEA — 세션 로그 (Read + Notify) */
};

/* ===== GATT 핸들 테이블 ===== */
/* 각 Characteristic마다 3개 핸들: decl + value + (optional) CCC descriptor */
#define HANDLE_COUNT    (1 + CHAR_COUNT * 3)  /* 1(service) + 9*(decl+val+ccc) = 28 */

static uint16_t handle_table[HANDLE_COUNT];

/* 핸들 인덱스 헬퍼 */
#define SVC_HANDLE              0
#define CHAR_DECL(i)            (1 + (i) * 3)
#define CHAR_VAL(i)             (2 + (i) * 3)
#define CHAR_CCC(i)            (3 + (i) * 3)

/* ===== 내부 상태 ===== */
static struct {
    bool connected;
    uint16_t conn_id;
    uint16_t gatts_if;
    bool registered;

    /* Notify 변화 감지용 캐시 */
    uint8_t last_state;
    uint8_t last_mode;
    uint8_t last_intensity;
    uint8_t last_battery;
    int8_t  last_temp;
    uint8_t last_safety_state;
    uint8_t last_error_code;
    bool last_session_active;   /* 세션 종료 감지용 */
} ctx;

/* ===== Characteristic 속성 정의 ===== */

/* 표준 UUID 상수 */
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_decl_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t ccc_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

/* Characteristic 속성 */
static const uint8_t prop_read            = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t prop_read_notify     = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t prop_read_write_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE |
                                              ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t prop_read_write      = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;

/* CCC 초기값 (Notify 비활성) */
static uint16_t ccc_val_default = 0x0000;

/* Characteristic 값 버퍼 (초기값) */
static uint8_t val_device_info[3] = { FW_MAJOR, FW_MINOR, FW_PATCH };
static uint8_t val_device_state = 0;
static uint8_t val_mode = 0;
static uint8_t val_intensity = 1;
static uint8_t val_led_color[3] = { 255, 100, 0 };
static uint8_t val_battery = 100;
static uint8_t val_temperature = 25;
static uint8_t val_safety[2] = { 0, 0 };
static uint8_t val_error_log[10] = { 0 };
static uint8_t val_session_log[18] = { 0 };  /* session_log_t packed = 18 bytes */

/* 각 Characteristic의 16-bit UUID */
static uint16_t char_uuids[CHAR_COUNT];

/* ===== GATT 속성 테이블 ===== */

static esp_gatts_attr_db_t gatt_db[HANDLE_COUNT];

static void build_gatt_db(void)
{
    int idx = 0;

    /* [0] Service Declaration */
    gatt_db[idx].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    gatt_db[idx].att_desc.uuid_length = ESP_UUID_LEN_16;
    gatt_db[idx].att_desc.uuid_p = (uint8_t *)&primary_service_uuid;
    gatt_db[idx].att_desc.perm = ESP_GATT_PERM_READ;
    gatt_db[idx].att_desc.max_length = sizeof(SERVICE_UUID);
    gatt_db[idx].att_desc.length = sizeof(SERVICE_UUID);
    gatt_db[idx].att_desc.value = (uint8_t *)SERVICE_UUID;
    idx++;

    /* Characteristic UUID 배열 초기화 */
    for (int i = 0; i < CHAR_COUNT; i++) {
        char_uuids[i] = CHAR_UUID_BASE + i;
    }

    /* 속성 정의 헬퍼 구조체 */
    struct char_def {
        const uint8_t *prop;
        uint8_t *value;
        uint16_t max_len;
        uint16_t cur_len;
        bool has_ccc;
    };

    struct char_def defs[CHAR_COUNT] = {
        [CHAR_DEVICE_INFO]   = { &prop_read,               val_device_info, 3,  3,  false },
        [CHAR_DEVICE_STATE]  = { &prop_read_notify,         &val_device_state, 1, 1, true  },
        [CHAR_MODE]          = { &prop_read_write_notify,   &val_mode,       1,  1,  true  },
        [CHAR_INTENSITY]     = { &prop_read_write_notify,   &val_intensity,  1,  1,  true  },
        [CHAR_LED_COLOR]     = { &prop_read_write,          val_led_color,   3,  3,  false },
        [CHAR_BATTERY]       = { &prop_read_notify,         &val_battery,    1,  1,  true  },
        [CHAR_TEMPERATURE]   = { &prop_read_notify,         &val_temperature, 1, 1,  true  },
        [CHAR_SAFETY_STATUS] = { &prop_read_notify,         val_safety,      2,  2,  true  },
        [CHAR_ERROR_LOG]     = { &prop_read,                val_error_log,   10, 10, false },
        [CHAR_SESSION_LOG]   = { &prop_read_notify,         val_session_log, 18, 18, true  },
    };

    for (int i = 0; i < CHAR_COUNT; i++) {
        /* Characteristic Declaration */
        gatt_db[idx].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        gatt_db[idx].att_desc.uuid_length = ESP_UUID_LEN_16;
        gatt_db[idx].att_desc.uuid_p = (uint8_t *)&char_decl_uuid;
        gatt_db[idx].att_desc.perm = ESP_GATT_PERM_READ;
        gatt_db[idx].att_desc.max_length = 1;
        gatt_db[idx].att_desc.length = 1;
        gatt_db[idx].att_desc.value = (uint8_t *)defs[i].prop;
        idx++;

        /* Characteristic Value */
        gatt_db[idx].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        gatt_db[idx].att_desc.uuid_length = ESP_UUID_LEN_16;
        gatt_db[idx].att_desc.uuid_p = (uint8_t *)&char_uuids[i];
        gatt_db[idx].att_desc.perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
        gatt_db[idx].att_desc.max_length = defs[i].max_len;
        gatt_db[idx].att_desc.length = defs[i].cur_len;
        gatt_db[idx].att_desc.value = defs[i].value;
        idx++;

        /* CCC Descriptor (Notify 구독용) */
        gatt_db[idx].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        gatt_db[idx].att_desc.uuid_length = ESP_UUID_LEN_16;
        gatt_db[idx].att_desc.uuid_p = (uint8_t *)&ccc_uuid;
        gatt_db[idx].att_desc.perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
        gatt_db[idx].att_desc.max_length = sizeof(uint16_t);
        gatt_db[idx].att_desc.length = sizeof(uint16_t);
        gatt_db[idx].att_desc.value = (uint8_t *)&ccc_val_default;
        idx++;
    }
}

/* ===== Notify 전송 헬퍼 ===== */

/* GATT DB 값 업데이트 (Read 시 최신 값 반환되도록) */
static void update_attr_value(int char_idx, uint8_t *data, uint16_t len)
{
    if (!ctx.registered) return;
    uint16_t handle = handle_table[CHAR_VAL(char_idx)];
    esp_ble_gatts_set_attr_value(handle, len, data);
}

static void send_notify(int char_idx, uint8_t *data, uint16_t len)
{
    /* Read 시에도 최신 값이 반환되도록 GATT DB 업데이트 */
    update_attr_value(char_idx, data, len);

    if (!ctx.connected) return;

    uint16_t handle = handle_table[CHAR_VAL(char_idx)];
    esp_ble_gatts_send_indicate(ctx.gatts_if, ctx.conn_id, handle, len, data, false);
}

/* ===== Write 핸들러 (BLE 콜백 → event_queue) ===== */

static void handle_write(uint16_t handle, uint8_t *value, uint16_t len)
{
    /* Mode Write */
    if (handle == handle_table[CHAR_VAL(CHAR_MODE)] && len >= 1) {
        uint8_t mode = value[0];
        if (mode < MODE_COUNT) {
            LOG_INFO("BLE Write: mode=%d", mode);
            event_t evt = {
                .type = EVENT_BLE_MODE_CHANGE,
                .data = mode,
                .timestamp_ms = hal_timer_get_ms(),
            };
            event_queue_push(evt);
        }
        return;
    }

    /* Intensity Write */
    if (handle == handle_table[CHAR_VAL(CHAR_INTENSITY)] && len >= 1) {
        uint8_t intensity = value[0];
        if (intensity >= 1 && intensity <= 5) {
            LOG_INFO("BLE Write: intensity=%d", intensity);
            event_t evt = {
                .type = EVENT_BLE_INTENSITY_CHANGE,
                .data = intensity,
                .timestamp_ms = hal_timer_get_ms(),
            };
            event_queue_push(evt);
        }
        return;
    }

    /* LED Color Write */
    if (handle == handle_table[CHAR_VAL(CHAR_LED_COLOR)] && len >= 3) {
        LOG_INFO("BLE Write: LED color R=%d G=%d B=%d", value[0], value[1], value[2]);
        event_t evt = {
            .type = EVENT_BLE_LED_COLOR_CHANGE,
            .data = (value[0] << 16) | (value[1] << 8) | value[2],
            .timestamp_ms = hal_timer_get_ms(),
        };
        event_queue_push(evt);
        return;
    }
}

/* ===== GAP 이벤트 콜백 ===== */

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,   /* 20ms */
    .adv_int_max = 0x40,   /* 40ms */
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t adv_data[] = {
    0x02, 0x01, 0x06,                         /* Flags: General Discoverable + BR/EDR Not Supported */
    0x09, 0x09, 'O', 'p', 'e', 'n', 'G', 'l', 'o', 'w',  /* Complete Local Name */
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            esp_ble_gap_start_advertising(&adv_params);
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                LOG_INFO("BLE advertising started");
            } else {
                LOG_ERROR("BLE advertising failed: %d", param->adv_start_cmpl.status);
            }
            break;
        default:
            break;
    }
}

/* ===== GATTS 이벤트 콜백 ===== */

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT:
            ctx.gatts_if = gatts_if;
            ctx.registered = true;
            /* 광고 데이터 설정 */
            esp_ble_gap_config_adv_data_raw(adv_data, sizeof(adv_data));
            /* GATT 테이블 등록 */
            esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HANDLE_COUNT, 0);
            LOG_INFO("GATT registered (if=%d)", gatts_if);
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status == ESP_GATT_OK &&
                param->add_attr_tab.num_handle == HANDLE_COUNT) {
                memcpy(handle_table, param->add_attr_tab.handles,
                       sizeof(uint16_t) * HANDLE_COUNT);
                esp_ble_gatts_start_service(handle_table[SVC_HANDLE]);
                LOG_INFO("GATT service started (%d handles)", HANDLE_COUNT);
            } else {
                LOG_ERROR("GATT table create failed: status=%d, handles=%d",
                          param->add_attr_tab.status, param->add_attr_tab.num_handle);
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            ctx.connected = true;
            ctx.conn_id = param->connect.conn_id;
            LOG_INFO("BLE connected (conn_id=%d)", ctx.conn_id);
            {
                event_t evt = { .type = EVENT_BLE_CONNECTED, .data = 0,
                                .timestamp_ms = hal_timer_get_ms() };
                event_queue_push(evt);
            }
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ctx.connected = false;
            LOG_INFO("BLE disconnected (reason=0x%x)", param->disconnect.reason);
            {
                event_t evt = { .type = EVENT_BLE_DISCONNECTED, .data = 0,
                                .timestamp_ms = hal_timer_get_ms() };
                event_queue_push(evt);
            }
            /* 다시 광고 시작 */
            esp_ble_gap_start_advertising(&adv_params);
            break;

        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep) {
                handle_write(param->write.handle, param->write.value, param->write.len);
            }
            break;

        default:
            break;
    }
}

/* ===== 공개 함수 ===== */

void ble_init(void)
{
    memset(&ctx, 0, sizeof(ctx));

    /* GATT DB 구성 */
    build_gatt_db();

    /* Bluetooth 컨트롤러 초기화 */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret;

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) { LOG_ERROR("BT controller init failed: %s", esp_err_to_name(ret)); return; }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) { LOG_ERROR("BT controller enable failed: %s", esp_err_to_name(ret)); return; }

    ret = esp_bluedroid_init();
    if (ret) { LOG_ERROR("Bluedroid init failed: %s", esp_err_to_name(ret)); return; }

    ret = esp_bluedroid_enable();
    if (ret) { LOG_ERROR("Bluedroid enable failed: %s", esp_err_to_name(ret)); return; }

    /* 콜백 등록 */
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);

    /* GATT 앱 등록 (앱 ID = 0) */
    esp_ble_gatts_app_register(0);

    /* 디바이스 이름 설정 */
    esp_ble_gap_set_device_name(BLE_DEVICE_NAME);

    LOG_INFO("BLE service initialized (name='%s')", BLE_DEVICE_NAME);
}

void ble_update(void)
{
    if (!ctx.registered) return;

    /* FSM 상태 → Notify (연결 시) + GATT DB 갱신 (항상) */
    const device_context_t *fsm = fsm_get_context();
    uint8_t state = (uint8_t)fsm->current_state;
    if (state != ctx.last_state) {
        ctx.last_state = state;
        val_device_state = state;
        send_notify(CHAR_DEVICE_STATE, &val_device_state, 1);
    }

    /* 모드 → Notify */
    uint8_t mode = (uint8_t)fsm->current_mode;
    if (mode != ctx.last_mode) {
        ctx.last_mode = mode;
        val_mode = mode;
        send_notify(CHAR_MODE, &val_mode, 1);
    }

    /* 세기 → Notify */
    uint8_t intensity = fsm->intensity_level;
    if (intensity != ctx.last_intensity) {
        ctx.last_intensity = intensity;
        val_intensity = intensity;
        send_notify(CHAR_INTENSITY, &val_intensity, 1);
    }

    /* 배터리 → Notify (5% 단위 변화 시만) */
    if (battery_is_voltage_connected()) {
        uint8_t batt = battery_get_percent();
        if (batt / 5 != ctx.last_battery / 5) {
            ctx.last_battery = batt;
            val_battery = batt;
            send_notify(CHAR_BATTERY, &val_battery, 1);
        }
    }

    /* 온도 → Notify (1°C 단위 변화 시) */
    if (battery_is_temp_connected()) {
        int8_t temp = (int8_t)battery_get_temperature();
        if (temp != ctx.last_temp) {
            ctx.last_temp = temp;
            val_temperature = (uint8_t)temp;
            send_notify(CHAR_TEMPERATURE, &val_temperature, 1);
        }
    }

    /* 안전 상태 → Notify */
    uint8_t safety_state = (uint8_t)safety_get_status();
    uint8_t error_code = fsm->error_code;
    if (safety_state != ctx.last_safety_state || error_code != ctx.last_error_code) {
        ctx.last_safety_state = safety_state;
        ctx.last_error_code = error_code;
        val_safety[0] = safety_state;
        val_safety[1] = error_code;
        send_notify(CHAR_SAFETY_STATUS, val_safety, 2);
    }

    /* 세션 로그 → Notify (세션 종료 시 최종 데이터 전송) */
    bool session_active = session_log_is_active();
    if (ctx.last_session_active && !session_active) {
        /* 세션이 방금 종료됨 → NVS에서 최종 로그 읽어서 Notify */
        session_log_t log;
        if (session_log_get_latest(&log)) {
            memcpy(val_session_log, &log, sizeof(session_log_t));
            send_notify(CHAR_SESSION_LOG, val_session_log, sizeof(session_log_t));
        }
    } else if (session_active) {
        /* 세션 진행 중 → GATT DB만 갱신 (Read 시 최신 스냅샷 반환) */
        session_log_t log;
        if (session_log_get_current(&log)) {
            memcpy(val_session_log, &log, sizeof(session_log_t));
            update_attr_value(CHAR_SESSION_LOG, val_session_log, sizeof(session_log_t));
        }
    }
    ctx.last_session_active = session_active;
}

/* 미사용 — FSM에서 BLE 연결 상태 동기 조회 시 활성화 예정
bool ble_is_connected(void)
{
    return ctx.connected;
}
*/
