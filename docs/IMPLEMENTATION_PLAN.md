# OpenGlow 구현 계획서 v2

## 1. 프로젝트 구조 및 의존 관계

```
main.c
  └─ device_fsm (상태 머신 - 전체 흐름 제어)
       ├─ ems_controller (PWM 펄스 생성)
       │    └─ pid_controller (세기 전환 시 부드러운 트랜지션)
       ├─ led_controller (RGB LED 색상 제어)
       ├─ vibration_controller (진동 모터 PWM)
       ├─ safety_manager (안전 제어)
       │    ├─ battery_monitor에서 전압/온도 데이터 수신
       │    └─ [비상 시] GPIO 직접 차단 경로 보유
       ├─ battery_monitor (ADC 배터리/온도 측정)
       ├─ button_handler (버튼 디바운싱 + 길게/짧게 누름 판별)  ← [추가]
       ├─ skin_contact (피부 접촉 감지)  ← [추가]
       ├─ event_queue (모듈 간 이벤트 전달 큐)  ← [추가]
       └─ ble_service (BLE GATT 서버)
            └─ 앱에서 오는 명령 → event_queue → FSM으로 전달

hal/ (Hardware Abstraction Layer)  ← [추가]
  ├─ hal_gpio.h    → MCU: ESP-IDF GPIO / PC: mock
  ├─ hal_adc.h     → MCU: ESP-IDF ADC / PC: mock
  ├─ hal_pwm.h     → MCU: ESP-IDF LEDC / PC: mock
  ├─ hal_timer.h   → MCU: esp_timer / PC: mock
  └─ hal_nvs.h     → MCU: NVS flash / PC: file-based mock
```

**핵심 원칙**: 모듈 간 직접 호출을 최소화하고, FSM이 중앙에서 조율합니다.
백엔드의 이벤트 드리븐 아키텍처와 같은 사고방식입니다.

**추가 원칙**:
- 모든 하드웨어 접근은 HAL을 통해서만 수행 (PC 단위 테스트 가능)
- 모듈 간 통신은 event_queue를 통해 수행 (BLE 콜백 동기화 포함)
- safety_manager는 비상 시 FSM을 우회하여 GPIO 직접 차단 가능 (이중 안전)

---

## 2. 모듈별 구현 계획

### 2.0 event_queue (이벤트 큐) — Phase 1에서 FSM과 함께 구현  ← [추가]

**역할**: 모듈 간 비동기 이벤트 전달. BLE 콜백 → 메인 루프 동기화의 핵심.

**핵심 함수**:
```c
void event_queue_init(void);
bool event_queue_push(event_t event);       // ISR-safe (critical section 사용)
bool event_queue_pop(event_t *event);        // 메인 루프에서 호출
bool event_queue_is_empty(void);
uint8_t event_queue_count(void);
```

**이벤트 정의**:
```c
typedef enum {
    // 버튼 이벤트 (물리 버튼 2개: 전원 버튼 + 모드 버튼)
    EVENT_BTN_POWER_SHORT,      // 전원 버튼 짧게 누름 (<2초)
    EVENT_BTN_POWER_LONG,       // 전원 버튼 길게 누름 (≥2초)
    EVENT_BTN_POWER_VLONG,      // 전원 버튼 매우 길게 누름 (≥3초, 강제 종료)
    EVENT_BTN_MODE_SHORT,       // 모드 버튼 짧게 누름 (모드 순환 / RUNNING 중 세기 증가)
    EVENT_BTN_MODE_LONG,        // 모드 버튼 길게 누름 (RUNNING 중 세기 감소)  ← [추가]

    // 안전 이벤트
    EVENT_SAFETY_TEMP_WARNING,  // 온도 경고 (40~50도)
    EVENT_SAFETY_TEMP_CRITICAL, // 온도 위험 (50도 이상) → 비상 차단 동시 발동
    EVENT_SAFETY_BATTERY_LOW,   // 배터리 부족 (10%)
    EVENT_SAFETY_BATTERY_CRITICAL, // 배터리 위험 (5%) → 강제 종료
    EVENT_SAFETY_AUTO_WARNING,  // 8분 경과 경고 (진동 피드백용)  ← [추가]
    EVENT_SAFETY_AUTO_TIMEOUT,  // 10분 자동 종료

    // 피부 접촉 이벤트
    EVENT_SKIN_CONTACT_ON,      // 피부 접촉 감지
    EVENT_SKIN_CONTACT_OFF,     // 피부 접촉 해제

    // 충전 이벤트  ← [추가]
    EVENT_CHARGE_CONNECTED,     // 충전기 연결 감지 (battery_monitor에서 push)
    EVENT_CHARGE_DISCONNECTED,  // 충전기 분리 감지
    EVENT_CHARGE_COMPLETE,      // 충전 완료

    // BLE 이벤트
    EVENT_BLE_CONNECTED,        // BLE 연결됨
    EVENT_BLE_DISCONNECTED,     // BLE 연결 끊김
    EVENT_BLE_MODE_CHANGE,      // 앱에서 모드 변경 요청
    EVENT_BLE_INTENSITY_CHANGE, // 앱에서 세기 변경 요청
    EVENT_BLE_LED_COLOR_CHANGE, // 앱에서 LED 색상 변경 요청

    EVENT_COUNT
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t data;              // 이벤트별 부가 데이터 (모드 값, 세기 값 등)
    uint32_t timestamp_ms;      // 이벤트 발생 시각
} event_t;
```

**큐 구현**: 링 버퍼 기반, 크기 16 (최악의 경우에도 오버플로 방지)
- push 시 critical section 진입 (ISR/BLE 콜백에서 안전하게 호출)
- **안전 이벤트 보호**: EVENT_SAFETY_* 이벤트는 오버플로 시에도 드롭하지 않음
  → 오버플로 시 가장 오래된 **비안전** 이벤트를 덮어씀  ← [수정]
- 일반 이벤트 오버플로 시 에러 카운터 증가 + LOG_WARN 출력

---

### 2.1 device_fsm (상태 머신) — 가장 먼저 구현

**역할**: 디바이스의 전체 동작 흐름을 제어하는 중앙 컨트롤러

**상태 정의**:
```
STATE_OFF         → 전원 꺼짐 (deep sleep)
STATE_IDLE        → 전원 켜짐, 대기 중
STATE_MODE_SELECT → 모드 선택 중 (LED로 현재 모드 표시)
STATE_RUNNING     → 출력 동작 중 (EMS/MICRO/THERMAL/PULSE)
STATE_PAUSED      → 일시 정지 (피부 접촉 해제 등)
STATE_CHARGING    → 충전 중 (출력 불가, LED로 충전 상태 표시)  ← [추가]
STATE_ERROR       → 에러 상태 (과열, 저전압 등)
STATE_SHUTDOWN    → 종료 시퀀스 (출력 안전 차단 후 OFF)
```

**상태 전이 조건**:
```
OFF → IDLE             : 전원 버튼 길게 누름 (2초)
OFF → CHARGING         : 충전기 연결 감지 (전원 버튼 없이)  ← [추가]
IDLE → MODE_SELECT     : 모드 버튼 짧게 누름
IDLE → CHARGING        : 충전기 연결 감지  ← [추가]
IDLE → SHUTDOWN        : 무조작 3분 경과 (자동 슬립)  ← [추가]
MODE_SELECT → MODE_SELECT : 모드 버튼 반복 누름 (순환: PULSE→MICRO→EMS→THERMAL→PULSE)  ← [추가]
MODE_SELECT → RUNNING  : 전원 버튼 짧게 누름 (시작)
MODE_SELECT → IDLE     : 무조작 10초 경과 (모드 선택 타임아웃)  ← [추가]
RUNNING → PAUSED       : 피부 접촉 해제 감지 (안전)
RUNNING → RUNNING      : 세기 변경 (모드 버튼 짧게 누름 → 세기 순환, 1차부터 활성화)  ← [추가]
RUNNING → CHARGING     : 충전기 연결 감지 → 출력 즉시 중단 후 CHARGING 전이  ← [추가]
PAUSED → RUNNING       : 피부 접촉 재감지 (3초 이내)
PAUSED → IDLE          : 피부 접촉 없이 10초 경과
PAUSED → IDLE          : 전원 버튼 짧게 누름 (수동 복귀)  ← [추가]
RUNNING → ERROR        : safety_manager가 위험 신호 발생
ERROR → IDLE           : 온도 정상 복귀 시 자동 복구 (TEMP_WARNING 해제 후 5초)  ← [추가, 결정]
ERROR → SHUTDOWN       : 복구 불가 에러 (EMERGENCY_SHUTDOWN) 시 자동 종료
CHARGING → IDLE        : 충전 완료 또는 충전기 분리  ← [추가]
CHARGING → CHARGING    : 충전 중 전원 버튼 → 무시 (충전 완료 대기)  ← [추가]
ANY(OFF/CHARGING 제외) → SHUTDOWN : 전원 버튼 매우 길게 누름 (3초)  ← [수정: OFF/CHARGING 제외]
SHUTDOWN → OFF         : 출력 안전 차단 완료 후
```
> **참고**: CHARGING 상태에서는 전원 버튼 길게 누름을 무시합니다.
> 충전 중 강제 종료하면 배터리 관리 IC의 충전 시퀀스가 중단될 수 있기 때문입니다.
> 충전기를 물리적으로 분리하면 IDLE로 전이 후 정상 종료 가능합니다.

**버튼-기능 매핑 (물리 버튼 2개)**:  ← [추가]
```
[전원 버튼]
  OFF 상태      : 길게(2초) → 전원 ON
  IDLE          : 짧게 → (없음), 길게(3초) → 전원 OFF
  MODE_SELECT   : 짧게 → RUNNING 시작
  RUNNING       : 짧게 → (없음), 길게(3초) → 전원 OFF
  PAUSED        : 짧게 → IDLE 복귀
  CHARGING      : 무시
  ANY(OFF/CHARGING 제외) : 매우 길게(3초) → 강제 종료

[모드 버튼]
  IDLE          : 짧게 → MODE_SELECT 진입
  MODE_SELECT   : 짧게 → 다음 모드 순환 (PULSE→MICRO→EMS→THERMAL→PULSE)
  RUNNING       : 짧게 → 세기 순환 (1→2→3→4→5→1)
  기타 상태      : 무시
```
> 물리 버튼이 2개(전원, 모드)뿐이므로, 모드 버튼이 RUNNING 중에는 세기 조절 역할을 겸함.
> 앱(BLE)을 통해서는 모드/세기를 각각 독립적으로 변경 가능.

**구현 방식**: switch-case 기반 FSM + 이벤트 큐 소비
- 각 상태에 `on_enter()`, `on_update()`, `on_exit()` 콜백
- 상태 전이 시 이전 상태의 on_exit → 새 상태의 on_enter 순서 보장
- `fsm_update()`에서 event_queue를 drain하며 이벤트 처리
- 전이 이벤트는 event_type_t enum 사용 (event_queue와 통합)

**데이터 구조**:
```c
typedef struct {
    device_state_t current_state;
    device_state_t previous_state;     // ← [추가] 이전 상태 추적 (디버그/복구용)
    device_mode_t current_mode;        // PULSE, MICRO, EMS, THERMAL
    uint8_t intensity_level;           // 1~5
    uint32_t running_start_time_ms;    // 사용 시작 시각 (세션 로깅용)
    uint32_t total_session_time_ms;    // 누적 사용 시간
    uint32_t state_enter_time_ms;      // ← [추가] 현재 상태 진입 시각 (타임아웃 계산용)
    uint32_t last_interaction_ms;      // ← [추가] 마지막 사용자 입력 시각 (자동 슬립용)
    bool skin_contact;                 // 피부 접촉 여부
    bool is_charging;                  // ← [추가] 충전 중 여부
    uint8_t error_code;                // ← [추가] 현재 에러 코드
} device_context_t;
```

**에러 코드 체계**:  ← [추가]
```c
typedef enum {
    ERR_NONE              = 0x00,
    ERR_TEMP_WARNING      = 0x01,  // 복구 가능
    ERR_TEMP_CRITICAL     = 0x02,  // 복구 불가 → SHUTDOWN
    ERR_BATTERY_LOW       = 0x10,  // 복구 가능 (충전 시)
    ERR_BATTERY_CRITICAL  = 0x11,  // 복구 불가 → SHUTDOWN
    ERR_EMS_FAULT         = 0x20,  // 출력 이상 감지
    ERR_SENSOR_FAULT      = 0x30,  // ADC 읽기 실패
    ERR_WATCHDOG_RESET    = 0x40,  // watchdog에 의한 리셋
} error_code_t;
```

**검토 포인트** (결정 사항 포함):
- [x] 모든 상태에서 전원 버튼 길게 누름 → SHUTDOWN이 가능한가? → **예, ANY→SHUTDOWN 전이로 보장**
- [x] RUNNING 상태에서 모드 변경이 가능해야 하는가? → **1차: 불가. Phase 6 [23]에서 RUNNING 중 모드 전환 추가**
- [x] ERROR 상태에서 자동 복구 시나리오가 필요한가? → **예, TEMP_WARNING은 온도 정상화 후 5초 대기 뒤 IDLE로 복구. CRITICAL/EMERGENCY는 즉시 SHUTDOWN**
- [x] IDLE 자동 슬립이 필요한가? → **예, 3분 무조작 시 SHUTDOWN → OFF (배터리 절약)**
- [x] 충전 중 동작 정책? → **CHARGING 상태 추가. 출력 불가, LED로 충전 상태 표시만**

---

### 2.0.1 button_handler (버튼 디바운싱)  ← [추가]

**역할**: 물리 버튼의 채터링 제거, 짧게/길게 누름 판별, event_queue에 이벤트 push

**물리 버튼 구성**: 2개  ← [추가, 명시]
```
전원 버튼 (POWER): 전원 ON/OFF, 모드 확정(시작), 강제 종료
모드 버튼 (MODE):  상태에 따라 역할 변경
  - IDLE/MODE_SELECT → 모드 순환 (PULSE→MICRO→EMS→THERMAL)
  - RUNNING → 짧게 누름: 세기 증가 / 길게 누름: 세기 감소
```
> 물리 버튼 2개로 모든 조작을 커버하는 설계.
> RUNNING 상태에서는 모드 버튼의 역할이 세기 조절로 변경됨.
> 세기 변경은 1차부터 즉시 전환 방식으로 허용, Phase 5에서 PID 트랜지션 적용.

**핵심 함수**:
```c
void button_init(void);
void button_update(void);                  // 메인 루프에서 호출 (10ms 주기)
```

**디바운싱 로직**:
```
1. GPIO raw 읽기
2. 50ms 디바운스 (5회 연속 동일 값 확인)
3. 눌림 시작 시각 기록
4. 해제 시:
   - 50ms~2000ms → SHORT press 이벤트
   - 2000ms~3000ms → LONG press 이벤트 (전원 ON/OFF)
   - ≥3000ms → VERY LONG press 이벤트 (강제 종료)
5. 길게 누르는 동안 2초/3초 시점에서 즉시 이벤트 발생 (해제 안 기다림)
```

**데이터 구조**:
```c
typedef struct {
    hal_gpio_pin_t pin;
    bool current_state;
    bool last_raw_state;
    uint8_t debounce_counter;       // 연속 동일 값 카운터
    uint32_t press_start_ms;        // 눌림 시작 시각
    bool long_fired;                // LONG 이벤트 이미 발생했는지
    bool vlong_fired;               // VLONG 이벤트 이미 발생했는지
    event_type_t short_event;       // 이 버튼의 SHORT 이벤트 타입
    event_type_t long_event;        // 이 버튼의 LONG 이벤트 타입
    event_type_t vlong_event;       // 이 버튼의 VLONG 이벤트 타입
} button_t;
```

---

### 2.0.2 skin_contact (피부 접촉 감지)  ← [추가]

**역할**: 피부 접촉 여부 감지, event_queue에 접촉/해제 이벤트 push

**감지 방식**: TTP223 터치 센서 모듈 (GPIO 디지털 입력)  ← [변경: ADC → GPIO]
- TTP223이 내부에서 정전 용량 변화 감지 + 디바운싱 처리
- ESP32는 GPIO로 결과만 읽음: HIGH=접촉, LOW=비접촉
- 소프트웨어 디바운스 100ms 추가 (상태 변경 시 채터링 방지)
- skin_contact_is_active() 뒤에 구현을 숨겨, 향후 ADC 임피던스 방식으로 교체 가능

**핵심 함수**:
```c
void skin_contact_init(void);
void skin_contact_update(void);            // 메인 루프에서 호출
bool skin_contact_is_active(void);         // 현재 접촉 상태 조회
```

---

### 2.2 ems_controller (EMS 펄스 생성) — FSM 다음에 구현

**역할**: 모드별로 다른 PWM 파라미터로 EMS/마이크로커런트/서멀 펄스 생성

**핵심 함수**:
```c
void ems_init(void);
void ems_set_mode(device_mode_t mode);             // 모드별 주파수 설정
void ems_set_intensity(uint8_t level);             // 세기 변경 (PID 트랜지션)
void ems_set_output_limit(float limit);            // ← [추가] safety_manager의 출력 제한 적용
void ems_start(void);                              // 출력 시작
void ems_stop(void);                               // 출력 중단 (즉시)
void ems_emergency_stop(void);                     // ← [추가] 비상 차단 (GPIO 직접 LOW)
void ems_update(void);                             // 메인 루프에서 호출 (PID 업데이트)
float ems_get_current_duty(void);                  // 현재 듀티비 조회 (BLE 전송용)
bool ems_is_active(void);                          // ← [추가] 출력 중 여부
```

**모드별 PWM 파라미터** (config.h에서 정의):
```
모드         | 주파수      | 듀티비 범위    | ESP32 구현            | 전공 지식
------------+------------+--------------+---------------------+------------------
PULSE     | 1kHz       | 10%~50%      | LEDC 타이머 (13bit)  | 일렉트로포레이션 원리
MICRO (마이크로커런트)| 10Hz       | 5%~30%       | GPIO 소프트웨어 토글  | 저주파 세포 자극
EMS (근육)   | 3kHz       | 15%~60%      | LEDC 타이머 (13bit)  | 중주파 근육 수축 [동역학]
THERMAL     | 5kHz       | 10%~45%      | LEDC 타이머 (13bit)  | 고주파 모공 케어
```

> **[추가] ESP32 LEDC 주파수 제한 분석**:
> ESP32 LEDC의 최소 주파수는 타이머 해상도에 따라 달라짐.
> 13bit 해상도에서 최소 ~1220Hz이므로, MICRO 모드(10Hz)는 LEDC로 직접 구현 불가.
> → **MICRO 모드는 소프트웨어 타이머 + GPIO 토글로 구현** (10Hz는 충분히 소프트웨어로 제어 가능)
> → 나머지 모드는 LEDC 하드웨어 PWM 사용

**세기 전환 (PID 트랜지션)**:
- 세기 1→3 변경 시 듀티비를 즉시 바꾸지 않고 PID로 부드럽게 전환
- 목표 듀티비까지 약 200~500ms에 걸쳐 도달
- 오버슈트 5% 이내로 제한 (사용자가 갑자기 강한 자극 느끼는 것 방지)
- **[추가]** output_limit 적용: 최종 듀티비 = PID 출력 × output_limit

**모드 전환 절차**:  ← [추가, 확정]
```
1. ems_stop() → 현재 PWM 듀티비를 0으로 즉시 설정
2. 10ms 대기 (하드웨어 안정화)
3. ems_set_mode(new_mode) → 새 주파수/해상도 설정
4. ems_start() → 새 모드의 세기 1부터 시작 (안전)
```

**PID 컨트롤러** (별도 유틸리티):
```c
typedef struct {
    float kp, ki, kd;
    float integral;
    float integral_limit;           // ← [추가] anti-windup 제한값
    float prev_error;
    float output_min, output_max;
} pid_controller_t;

float pid_update(pid_controller_t *pid, float target, float current, float dt);
void pid_reset(pid_controller_t *pid);  // integral, prev_error 초기화
```

> **[추가] Anti-windup**: integral 값이 integral_limit을 초과하면 클램핑.
> 세기 전환 시 PID 출력이 output_max에 오래 머물면 integral이 무한히 쌓여
> 목표 도달 후에도 오버슈트가 발생하는 문제 방지.

**검토 포인트** (결정 사항 포함):
- [x] PID 파라미터(Kp, Ki, Kd)는 Python 시뮬레이션으로 먼저 튜닝 필요 → **Phase 5 [17]에서 수행**
- [x] ESP32의 LEDC PWM 타이머 해상도와 주파수 제한 확인 → **MICRO 모드(10Hz)는 소프트웨어 토글로 구현**
- [x] 모드 전환 시 현재 PWM을 먼저 0으로 내리고 새 주파수를 설정해야 하는가? → **예, 위 절차 확정**

---

### 2.3 led_controller (RGB LED 제어)

**역할**: WS2812B NeoPixel LED의 색상, 밝기, 패턴 제어

**핵심 함수**:
```c
void led_init(void);
void led_set_color(uint8_t r, uint8_t g, uint8_t b);   // 단색 설정
void led_set_mode_color(device_mode_t mode);             // 모드별 기본색 적용
void led_set_brightness(uint8_t brightness);             // 밝기 조절 (0~255)
void led_blink(uint16_t on_ms, uint16_t off_ms);        // 깜빡임 (대기/에러 표시)
void led_breathe(uint16_t period_ms);                    // 숨쉬기 효과 (대기 상태)
void led_fade_out(uint16_t duration_ms);                 // ← [추가] 페이드아웃 (종료 시)
void led_show_battery_level(uint8_t percent);            // ← [추가] 충전 상태 표시
void led_update(void);                                   // 메인 루프에서 호출
void led_off(void);
```

**모드별 기본 색상 (RGB)**:  ← [추가, 구체화]
```
PULSE   → (255, 100, 0)    주황색  — 강한 에너지 느낌
MICRO     → (0, 200, 255)    하늘색  — 부드러운 마이크로커런트
EMS       → (0, 255, 100)    초록색  — 근육/건강 이미지
THERMAL   → (200, 0, 255)    보라색  — 고주파 모공 케어
```

**상태별 LED 동작**:
```
OFF         → LED 전체 소등
IDLE        → 현재 모드 색상으로 숨쉬기 효과 (2초 주기)
MODE_SELECT → 선택 중인 모드 색상으로 빠른 깜빡임 (200ms on/200ms off)
RUNNING     → 현재 모드 색상 고정 점등 (밝기 = 세기 레벨에 비례)
PAUSED      → 현재 모드 색상 느린 깜빡임 (500ms on/500ms off)
CHARGING    → 배터리 잔량에 따라 빨강→주황→초록 그라데이션, 숨쉬기  ← [추가]
ERROR       → 빨간색 빠른 깜빡임 (100ms on/100ms off)
SHUTDOWN    → 현재 색상에서 페이드아웃 (1초)
배터리 부족  → RUNNING 중 주기적 빨간색 1회 깜빡임 (10초 간격)
```

**구현 방식**: ESP32 RMT 드라이버 사용 (확정)  ← [추가, 결정]
> WS2812B의 타이밍 요구사항(T0H=400ns, T1H=800ns)을 소프트웨어로 맞추기 어려우므로
> ESP32의 RMT(Remote Control Transceiver) 드라이버를 사용.
> ESP-IDF에 led_strip 컴포넌트가 내장되어 있어 바로 사용 가능.

**검토 포인트** (결정 사항 포함):
- [x] WS2812B 드라이버 → **ESP32 RMT + led_strip 컴포넌트 사용**
- [ ] 앱에서 사용자가 LED 색상을 커스텀할 수 있어야 하는가? → **1차에서는 모드별 고정색만, Phase 5에서 BLE Characteristic 추가**
- [x] 밝기 조절이 PWM이 아닌 RGB 값 스케일링인 점 주의 → **확인, RGB 값에 brightness/255를 곱하는 방식**

---

### 2.4 vibration_controller (진동 모터 제어)

**역할**: coin vibration motor의 PWM 제어

**핵심 함수**:
```c
void vibration_init(void);
void vibration_set_intensity(uint8_t level);    // 1~5 → 듀티비 매핑
void vibration_pulse(uint16_t on_ms, uint16_t off_ms, uint8_t count);  // 지정 횟수 진동
void vibration_start(void);
void vibration_stop(void);
void vibration_update(void);
```

**사용 시나리오**:
- 전원 ON: 중간 세기 진동 1회 (200ms)
- 모드 전환 시: 짧은 진동 1회 (100ms, 햅틱 피드백)
- 세기 변경 시: 짧은 진동 (50ms × 변경된 세기 레벨)
- 에러/경고 시: 강한 진동 3회 (200ms on, 100ms off)
- 자동 종료 임박 시 (8분 경과): 약한 진동 2회 (경고)
- 전원 OFF: 짧은 진동 1회 (100ms)

**듀티비 매핑**:  ← [추가]
```
세기 1 → 30% 듀티비 (coin motor 최소 구동 듀티비 확인 필요)
세기 2 → 45%
세기 3 → 60%
세기 4 → 75%
세기 5 → 90%
```
> coin motor는 최소 구동 전압 이하에서 회전하지 않으므로,
> 최소 듀티비를 실측하여 config.h에 VIBRATION_MIN_DUTY로 정의.

**검토 포인트** (결정 사항 포함):
- [x] coin motor의 응답 시간 고려 → **최소 듀티비 30%에서 시작, 실측 후 조정**
- [ ] 진동공학: 모터 고유 진동수 근처에서 구동해야 효율적 → **프로토타입 모터 스펙 확인 후 PWM 주파수 결정**

---

### 2.5 safety_manager (안전 제어) — 이중 안전 구조 확정

**역할**: 과열 보호, 자동 전원 차단, 연속샷 방지, 저전압 보호, **비상 차단**

**핵심 함수**:
```c
void safety_init(void);
void safety_update(void);                           // 메인 루프에서 주기적 호출
safety_status_t safety_get_status(void);             // 현재 안전 상태
bool safety_can_fire_shot(void);                     // 연속샷 쿨다운 체크
void safety_record_shot(void);                       // 샷 발사 기록
float safety_get_output_limit(void);                 // 온도 기반 출력 제한 비율 (0.0~1.0)

// ← [추가] 비상 차단 함수 (FSM 우회)
void safety_emergency_shutdown(void);                // GPIO 직접 차단 + 모든 출력 강제 OFF
```

**이중 안전 구조 (확정)**:  ← [추가, Q1 해결]
```
정상 경로 (Level 1):
  safety_update() → 위험 감지 → event_queue에 EVENT_SAFETY_* push
  → FSM이 이벤트 소비 → ems_stop() + 상태 전이

비상 경로 (Level 2):
  safety_update() → CRITICAL 감지 (50도 이상, 배터리 5% 이하)
  → safety_emergency_shutdown() 직접 호출
  → hal_gpio_write(EMS_ENABLE_PIN, LOW) ← GPIO 직접 차단
  → ems_emergency_stop() 호출
  → event_queue에 EVENT_SAFETY_TEMP_CRITICAL push (FSM에도 통보)
```
> **원칙**: Level 2 비상 차단은 FSM 상태와 무관하게 즉시 실행.
> FSM에 버그가 있어도 하드웨어 레벨에서 출력이 차단됨.

**안전 로직 상세**:

1. **과열 보호** (구간별 출력 제한):
   ```
   온도 < 40도  → output_limit = 1.0 (제한 없음)
   40~45도      → output_limit = 선형 감소 (1.0 → 0.5)  ← [변경: PID → 선형 보간, 1차에서 단순화]
   45~50도      → output_limit = 0.5 (50% 강제 제한) + EVENT_SAFETY_TEMP_WARNING
   50도 이상    → output_limit = 0.0 + safety_emergency_shutdown() + EVENT_SAFETY_TEMP_CRITICAL
   ```
   - 온도 센서 노이즈는 이동평균 필터로 제거 (battery_monitor에서 처리)
   - **[추가]** 온도 센서 없는 프로토타입: 타이머 기반 추정 (RUNNING 시작 후 경과 시간 × 추정 발열 계수)

2. **자동 전원 차단**: RUNNING 상태 10분 경과 시 EVENT_SAFETY_AUTO_TIMEOUT
   - **[추가]** 8분 경과 시 EVENT_SAFETY_AUTO_WARNING 이벤트 push → FSM이 vibration_pulse() 호출
     (safety_manager가 vibration_controller를 직접 호출하지 않음 — FSM 경유 원칙 유지)

3. **연속샷 방지**: 마지막 샷 이후 0.5초 이내 재발사 차단

4. **저전압 보호**: 배터리 10% 이하 시 출력 제한(output_limit=0.7), 5% 이하 시 safety_emergency_shutdown()

**output_limit 전달 방식 (확정)**:  ← [추가, 결정]
> FSM이 safety_get_output_limit()을 주기적으로 호출하여 ems_set_output_limit()에 전달.
> FSM 경유로 데이터 흐름을 명확히 하되, 비상 시에는 safety가 ems_emergency_stop()을 직접 호출.

**데이터 구조**:
```c
typedef enum {
    SAFETY_OK,
    SAFETY_TEMP_WARNING,
    SAFETY_TEMP_CRITICAL,
    SAFETY_BATTERY_LOW,
    SAFETY_BATTERY_CRITICAL,
    SAFETY_EMERGENCY_SHUTDOWN
} safety_status_t;

typedef struct {
    safety_status_t status;
    float current_temp;
    float output_limit;              // 0.0~1.0
    uint32_t last_shot_time_ms;
    uint32_t running_elapsed_ms;
    uint32_t warning_vibration_sent;  // ← [추가] 8분 경고 진동 발송 여부
    bool emergency_triggered;         // ← [추가] 비상 차단 발동 여부
} safety_context_t;
```

---

### 2.6 battery_monitor (배터리/온도 ADC)

**역할**: ADC로 배터리 전압과 온도 센서 값을 주기적으로 읽고 가공

**핵심 함수**:
```c
void battery_init(void);
void battery_update(void);                  // 주기적 측정 (내부에서 5초 간격 체크)
uint8_t battery_get_percent(void);           // 배터리 잔량 (0~100)
float battery_get_voltage(void);             // 원시 전압
float battery_get_temperature(void);         // 온도 (섭씨)
bool battery_is_charging(void);              // 충전 중 여부
bool battery_is_valid(void);                 // ← [추가] ADC 읽기 성공 여부 (센서 고장 감지)
```

**ADC 읽기 및 필터링**:
```
1. ADC raw value 읽기 (0~4095, 12bit)
2. 유효성 검증: raw == 0 또는 raw == 4095이면 센서 고장 의심  ← [추가]
3. 이동평균 필터 적용 (최근 16개 샘플 평균) [수치해석]
4. 전압 변환: voltage = raw * 3.3 / 4095 * 분압비
5. 퍼센트 환산: lookup table 기반 보간 (리튬 배터리 비선형 방전 곡선)
   → 1차에서는 선형 보간(4.2V=100%, 3.0V=0%), Phase 5에서 lookup table로 교체
```

**호출 주기 구현**:  ← [추가, 명시]
```c
void battery_update(void) {
    uint32_t now = hal_timer_get_ms();

    // 충전 상태 GPIO는 매 루프마다 확인 (즉시 응답 필요)
    bool charging_now = hal_gpio_read(PIN_CHARGE_STAT);
    if (charging_now != prev_charging_state) {
        if (charging_now) event_queue_push(EVENT_CHARGE_CONNECTED);
        else              event_queue_push(EVENT_CHARGE_DISCONNECTED);
        prev_charging_state = charging_now;
    }

    // ADC 읽기는 5초 간격
    if (now - last_update_ms < BATTERY_UPDATE_INTERVAL_MS) return;
    last_update_ms = now;
    // ... 실제 ADC 읽기 및 필터링 (전압, 온도)
}
```
> 메인 루프(10ms)에서 매번 호출되지만, 내부에서 5초 간격을 체크하여 스킵.
> safety_update()보다 먼저 호출되어야 최신 온도/전압 데이터를 safety가 참조 가능.

**충전 상태 변화 감지**:  ← [추가]
> battery_update() 내부에서 PIN_CHARGE_STAT GPIO를 읽어 충전 상태 변화를 감지.
> 상태가 변경되면 EVENT_CHARGE_CONNECTED / EVENT_CHARGE_DISCONNECTED / EVENT_CHARGE_COMPLETE를
> event_queue에 push. FSM이 이벤트를 소비하여 CHARGING 상태 전이를 수행.
> 충전 상태 GPIO는 5초 간격이 아닌 매 루프(10ms)마다 확인 (즉시 응답 필요).

**NTC 서미스터 온도 변환**:  ← [추가]
```c
// Steinhart-Hart 방정식 (NTC 10K 서미스터 가정)
// 1/T = 1/T0 + (1/B) * ln(R/R0)
// T0 = 298.15K (25°C), B = 3950 (일반적인 NTC 10K), R0 = 10kΩ
float adc_to_temperature(uint16_t adc_raw) {
    float resistance = NTC_SERIES_RESISTOR / (4095.0f / adc_raw - 1.0f);
    float steinhart = log(resistance / NTC_NOMINAL_R) / NTC_B_COEFFICIENT;
    steinhart += 1.0f / (NTC_NOMINAL_TEMP + 273.15f);
    return (1.0f / steinhart) - 273.15f;
}
```

**검토 포인트** (결정 사항 포함):
- [x] ESP32 ADC 비선형성 → **1차에서는 무시, 문제 발생 시 espefuse ADC 보정 사용**
- [x] 온도 센서 종류 → **NTC 10K 서미스터 가정, Steinhart-Hart 방정식 사용**
- [x] 충전 감지 → **충전 IC의 STAT 핀을 GPIO로 읽어 판단 (전압 변화율은 불안정)**

---

### 2.7 ble_service (BLE GATT 서버)

**역할**: 스마트폰 앱과 BLE 통신. 디바이스 상태 노출 + 앱 명령 수신

**GATT 서비스 설계** (REST API 설계와 동일한 사고방식):
```
Service: OpenGlow Device (UUID: 12345678-...-abc)
├── Characteristic: Device Info (000)           ← [추가]
│   └── Read   → 펌웨어 버전 (3 bytes: major.minor.patch)
├── Characteristic: Device State (001)          ← [추가]
│   ├── Read   → 현재 FSM 상태 (1 byte)
│   └── Notify → 상태 변경 시 앱에 알림
├── Characteristic: Mode (002)
│   ├── Read   → 현재 모드 (0=PULSE, 1=MICRO, 2=EMS, 3=THERMAL)
│   ├── Write  → 모드 변경 요청
│   └── Notify → 모드 변경 시 앱에 알림
├── Characteristic: Intensity (003)
│   ├── Read   → 현재 세기 (1~5)
│   ├── Write  → 세기 변경 요청
│   └── Notify → 세기 변경 시 알림
├── Characteristic: LED Color (004)
│   ├── Read   → 현재 RGB 값 (3 bytes)
│   └── Write  → 커스텀 LED 색상 설정
├── Characteristic: Battery (005)
│   ├── Read   → 배터리 잔량 (0~100)
│   └── Notify → 잔량 변경 시 알림 (5% 단위)
├── Characteristic: Session Log (006)
│   ├── Read   → 현재 세션 데이터 (packed struct, 16 bytes)
│   └── Notify → 세션 종료 시 요약 데이터 전송
├── Characteristic: Safety Status (007)
│   ├── Read   → 안전 상태 코드 + 에러 코드 (2 bytes)
│   └── Notify → 안전 이벤트 발생 시 알림
└── Characteristic: Error Log (008)             ← [추가]
    └── Read   → 최근 에러 코드 배열 (최대 10개, NVS에서 로드)
```

**앱 → 디바이스 명령 처리 흐름 (event_queue 경유)**:  ← [변경, 확정]
```
1. 앱이 Mode Characteristic에 Write (예: 0x02 = EMS)
2. BLE 콜백 함수 호출됨 (ESP-IDF BLE 태스크에서 실행)
3. 콜백이 event_queue에 EVENT_BLE_MODE_CHANGE push (ISR-safe)
4. 메인 루프의 fsm_update()에서 이벤트 소비
5. FSM이 상태에 따라 처리:
   - IDLE/MODE_SELECT → 모드 변경 적용 + Notify(새 모드 값)
   - RUNNING/PAUSED → 변경 거부 + Notify(현재 모드 값, 변경 없음)
6. 앱은 Notify 값과 자신이 Write한 값을 비교하여 성공/거부 판단
```
> BLE 콜백은 ESP-IDF의 별도 태스크에서 실행되므로,
> event_queue를 통해 메인 루프와 동기화. 직접 FSM 함수 호출 금지.
>
> **거부 응답 규칙**: Write 거부 시 BLE 프로토콜의 ATT Error Response가 아닌,
> 현재 값을 Notify로 재전송하는 방식. 앱 UX에서 "현재 모드로 되돌림"으로 처리.

**세션 로그 데이터 구조** (Characteristic 006):
```c
typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t start_time;         // epoch seconds
    uint32_t duration_sec;       // 총 사용 시간
    uint8_t  mode;               // 주로 사용한 모드
    uint8_t  avg_intensity;      // 평균 세기
    uint16_t shot_count;         // 총 샷 수
    uint8_t  max_temp;           // 최고 온도
    uint8_t  min_battery;        // 최저 배터리
} session_log_t;                 // 16 bytes → BLE 기본 MTU(20 bytes) 내 전송 가능
```

**세션 로그 저장**:  ← [추가]
> 세션 종료 시 NVS(Non-Volatile Storage)에 최근 10개 세션 저장.
> BLE 연결이 없어도 데이터 유실 없음. 다음 연결 시 앱에서 Read로 조회.

**검토 포인트** (결정 사항 포함):
- [x] BLE 연결 끊김 시 진행 중인 세션 → **디바이스 자체 동작 유지, NVS에 세션 저장**
- [x] 여러 앱 동시 연결 → **1:1 연결만 허용**
- [x] RUNNING 중 앱 모드 변경 → **1차: 불허, 현재 모드 Notify로 거부 응답**
- [x] BLE 보안 → **프로토타입: Just Works, 향후 Passkey**
- [x] Firmware Version Characteristic → **추가 (OTA 대비)**

---

### 2.8 main.c (엔트리포인트)

**역할**: 시스템 초기화, 메인 루프 실행, watchdog 설정

**초기화 순서** (의존관계에 따른 순서):
```c
void app_main(void) {
    // 0. 로깅 시스템 초기화  ← [추가]
    debug_log_init();
    LOG_INFO("OpenGlow firmware v%d.%d.%d starting...", FW_MAJOR, FW_MINOR, FW_PATCH);

    // 1. HAL 초기화  ← [변경]
    hal_gpio_init();
    hal_adc_init();
    hal_pwm_init();
    hal_nvs_init();

    // 2. 이벤트 큐 초기화 (다른 모듈이 사용)  ← [추가]
    event_queue_init();

    // 3. 센서/입력 초기화 (다른 모듈이 참조)
    battery_init();
    button_init();          // ← [추가]
    skin_contact_init();    // ← [추가]

    // 4. 출력 모듈 초기화
    ems_init();
    led_init();
    vibration_init();

    // 5. 안전 관리자 초기화 (출력 모듈 이후, 비상 차단 GPIO 설정)
    safety_init();

    // 6. 상태 머신 초기화 (모든 모듈 이후)
    fsm_init();

    // 7. BLE 서비스 초기화 (FSM 이후, 앱 명령 수신 준비)
    ble_init();

    // 8. Watchdog 타이머 설정  ← [추가]
    hal_watchdog_init(WATCHDOG_TIMEOUT_MS);  // 5000ms

    LOG_INFO("All modules initialized. Entering main loop.");

    // 9. 메인 루프
    while (1) {
        hal_watchdog_feed();        // ← [추가] watchdog 리셋

        // 입력 수집 (센서 → 이벤트 생성)
        button_update();            // ← [추가] 버튼 디바운싱 + 이벤트 push
        skin_contact_update();      // ← [추가] 피부 접촉 감지 + 이벤트 push
        battery_update();           // ADC 읽기 (내부에서 5초 간격 체크)

        // 안전 체크 (최신 센서 데이터 기반)
        safety_update();            // 위험 감지 → 이벤트 push / 비상 차단

        // 중앙 제어 (이벤트 소비 + 상태 전이 + 출력 모듈 제어)
        fsm_update();               // event_queue drain + 상태 머신 업데이트

        // 출력 업데이트 (FSM이 설정한 값 반영)
        ems_update();               // PID 업데이트
        led_update();               // LED 패턴 업데이트
        vibration_update();         // 진동 패턴 업데이트

        // BLE 상태 동기화
        ble_update();               // Notify 전송 등

        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms 주기 (100Hz)
    }
}
```

**메인 루프 호출 순서 의도**:  ← [추가, 명시]
```
입력 수집 → 안전 체크 → FSM 판단 → 출력 반영 → BLE 동기화
```
> 이 순서를 지켜야 한 루프 사이클 내에서 "센서 읽기 → 위험 판단 → 대응"이
> 10ms 이내에 완료됨. 순서가 바뀌면 1 사이클(10ms) 지연 발생.

**디버그 로깅 전략**:  ← [추가]
```c
// config.h에서 로그 레벨 설정
#define LOG_LEVEL LOG_LEVEL_INFO    // DEBUG, INFO, WARN, ERROR

// 사용 예
LOG_DEBUG("PID update: target=%.2f, current=%.2f, output=%.2f", target, current, output);
LOG_INFO("State transition: %s → %s", state_name(prev), state_name(next));
LOG_WARN("Temperature warning: %.1f°C", temp);
LOG_ERROR("Emergency shutdown triggered! Temp=%.1f°C", temp);
```
> UART 시리얼 출력 사용. 릴리즈 빌드에서는 LOG_LEVEL을 WARN으로 설정하여 성능 영향 최소화.

**검토 포인트** (결정 사항 포함):
- [x] 메인 루프 주기 10ms → **PID(200~500ms 트랜지션)에 충분. BLE는 ESP-IDF가 별도 태스크 처리**
- [x] FreeRTOS 멀티태스크 필요? → **1차: 단일 루프 + event_queue. BLE 콜백은 ESP-IDF 자체 태스크에서 실행되고 event_queue로 동기화**
- [x] watchdog → **5초 타임아웃, 메인 루프 시작에서 feed. 행 발생 시 자동 리셋**

---

## 3. 구현 순서 (의존관계 기반)

```
Phase 1: 기반 구조 (예상 1주)
  ├─ [1] openglow_config.h (설정값 정의 + 에러 코드 enum)
  ├─ [2] hal/ 레이어 정의 (인터페이스 + ESP32 구현 + PC mock)  ← [추가]
  ├─ [3] event_queue (링 버퍼 기반 이벤트 큐)  ← [추가]
  ├─ [4] button_handler (디바운싱 + 길게/짧게 판별)  ← [추가]
  ├─ [5] device_fsm (상태 머신 뼈대, 버튼 이벤트로 상태 전이)
  ├─ [6] debug_log (UART 시리얼 로깅)  ← [추가]
  └─ [7] main.c (초기화 + 메인 루프 뼈대 + watchdog)

Phase 2: 출력 모듈 (예상 1주)
  ├─ [8] ems_controller (PWM 출력, PID 없이 즉시 전환부터)
  ├─ [9] led_controller (단색 점등 + 모드별 색상)
  └─ [10] vibration_controller (단순 on/off + 펄스)

Phase 3: 센서 및 안전 (예상 1주)
  ├─ [11] skin_contact (피부 접촉 감지)  ← [추가]
  ├─ [12] battery_monitor (ADC 읽기 + 이동평균 필터)
  └─ [13] safety_manager (자동 꺼짐 타이머 + 비상 차단 + 저전압 보호)

Phase 4: 통신 (예상 1~2주)
  ├─ [14] ble_service (GATT 서버 기본 구조 + Device Info/State)
  ├─ [15] 앱에서 Read/Write 동작 확인 (nRF Connect)
  └─ [16] NVS 세션 로그 저장/조회  ← [추가]

Phase 5: 고도화 (예상 2주)
  ├─ [17] PID 트랜지션 (Python 시뮬레이션 → C 구현 + anti-windup)
  ├─ [18] LED 패턴 (숨쉬기, 깜빡임, 페이드아웃, 충전 표시)
  ├─ [19] 세션 로깅 및 BLE Notify
  ├─ [20] RUNNING 중 세기 변경에 PID 트랜지션 적용 (1차에서는 즉시 전환)
  ├─ [21] 배터리 lookup table 기반 비선형 보간  ← [추가]
  └─ [22] 문서화 (시뮬레이션 비교, 문제 해결 로그)

Phase 6: 보너스
  ├─ [23] RUNNING 중 모드 전환 (출력 0 → 모드 변경 → 출력 재개)
  ├─ [24] OTA 펌웨어 업데이트
  └─ [25] 앱 커스텀 LED 색상 (BLE Characteristic 추가)
```

---

## 4. 설계 결정 사항 (열린 질문 → 해결)

### Q1: FSM과 safety_manager의 관계 → ✅ 해결
**결정**: 이중 안전 구조 채택.
- Level 1 (정상): safety → event_queue → FSM → ems_stop()
- Level 2 (비상): safety → ems_emergency_stop() + hal_gpio_write(EMS_PIN, LOW) 직접 호출
- FSM에 버그가 있어도 Level 2가 하드웨어 레벨에서 출력 차단

### Q2: 메인 루프 vs FreeRTOS 멀티태스크 → ✅ 해결
**결정**: 단일 메인 루프 + event_queue.
- BLE 콜백은 ESP-IDF가 별도 태스크에서 실행
- 콜백 → event_queue.push() (critical section으로 thread-safe)
- 메인 루프의 fsm_update()에서 event_queue.pop()으로 이벤트 소비
- 멀티태스크 분리는 성능 문제 발생 시에만 고려

### Q3: RUNNING 중 모드/세기 변경 → ✅ 해결
**결정**:
- 세기 변경: **1차부터 허용** — 모드 버튼 짧게 누름으로 세기 순환 (1→2→...→5→1), PID 트랜지션은 Phase 5 [17]에서 추가 (1차에서는 즉시 전환)
- 모드 변경: Phase 6 [23]에서 허용 (출력 0 → 10ms 대기 → 새 모드 → 출력 재개)
- 1차에서는 모드 변경만 불허 (IDLE로 돌아가야 변경 가능), 세기 변경은 허용

### Q4: 프로토타입의 물리적 한계 → ✅ 유지
**결정**: LED 밝기/색상 + 오실로스코프 파형으로 동작 시각화.
- config.h에 `#define PROTOTYPE_MODE 1` 매크로 추가
- PROTOTYPE_MODE일 때 EMS 출력 핀을 LED 밝기 핀으로 매핑
- 문서에 "실제 제품에서는 여기에 EMS 드라이버 회로가 연결됨" 명시

### Q5: 테스트 전략 → ✅ 구체화
**결정**:
- **HAL 기반 단위 테스트**: hal/ mock 구현으로 PC에서 FSM, PID, safety 로직 테스트
  - CMake 빌드 시스템에서 `BUILD_TARGET=pc` 옵션으로 mock 링크
  - assert 기반 간단한 테스트 프레임워크 (Unity 또는 직접 구현)
- **통합 테스트**: UART 시리얼 로그로 상태 전이 확인
  - 로그 포맷: `[timestamp] [module] [level] message`
  - 예: `[12345] [FSM] [INFO] IDLE → MODE_SELECT (EVENT_BTN_MODE_SHORT)`
- **BLE 테스트**: nRF Connect 앱으로 GATT 서비스 검증
- **자동 테스트 스크립트**: Python + pyserial로 시리얼 로그 파싱하여 상태 전이 시퀀스 검증  ← [추가]

### Q6: ERROR 자동 복구 → ✅ 추가, 해결
**결정**:
- TEMP_WARNING (40~50도): 온도가 38도 이하로 내려가면 5초 대기 후 IDLE로 자동 복구
- TEMP_CRITICAL (50도 이상): 복구 불가, SHUTDOWN으로 전이
- BATTERY_LOW (10%): 충전 시작하면 CHARGING으로 전이
- BATTERY_CRITICAL (5%): 복구 불가, SHUTDOWN으로 전이
- 히스테리시스 적용: 진입 임계값과 복구 임계값을 다르게 설정하여 경계 지점에서의 떨림 방지

---

## 5. config.h 주요 설정값  ← [추가 섹션]

```c
// ===== 펌웨어 버전 =====
#define FW_MAJOR 0
#define FW_MINOR 1
#define FW_PATCH 0

// ===== 빌드 모드 =====
#define PROTOTYPE_MODE 1            // 1: 프로토타입, 0: 양산

// ===== 버튼 =====
#define BTN_DEBOUNCE_MS         50
#define BTN_SHORT_MAX_MS        2000
#define BTN_LONG_MS             2000
#define BTN_VLONG_MS            3000

// ===== FSM 타이머 =====
#define IDLE_AUTO_SLEEP_MS      180000  // 3분
#define MODE_SELECT_TIMEOUT_MS  10000   // 10초
#define PAUSE_TIMEOUT_MS        10000   // 10초
#define PAUSE_RESUME_MS         3000    // 3초 (피부 재접촉)
#define RUNNING_MAX_MS          600000  // 10분
#define RUNNING_WARNING_MS      480000  // 8분 (경고)

// ===== EMS PWM =====
#define EMS_PULSE_FREQ_HZ     1000
#define EMS_MICRO_FREQ_HZ          10      // 소프트웨어 토글
#define EMS_EMS_FREQ_HZ         3000
#define EMS_THERMAL_FREQ_HZ     5000
#define EMS_PWM_RESOLUTION_BITS 13      // 8192 단계

// ===== PID =====
#define PID_KP                  2.0f    // Python 시뮬레이션 후 조정
#define PID_KI                  0.5f
#define PID_KD                  0.1f
#define PID_INTEGRAL_LIMIT      100.0f  // anti-windup
#define PID_TRANSITION_TIME_MS  300     // 목표 전환 시간

// ===== 안전 =====
#define SAFETY_TEMP_WARNING_C   40.0f
#define SAFETY_TEMP_LIMIT_50_C  45.0f
#define SAFETY_TEMP_CRITICAL_C  50.0f
#define SAFETY_TEMP_RECOVER_C   38.0f   // 히스테리시스
#define SAFETY_TEMP_RECOVER_DELAY_MS 5000
#define SAFETY_BATTERY_LOW_PCT  10
#define SAFETY_BATTERY_CRIT_PCT 5
#define SAFETY_SHOT_COOLDOWN_MS 500

// ===== 배터리 =====
#define BATTERY_UPDATE_INTERVAL_MS  5000
#define BATTERY_FILTER_SIZE         16
#define BATTERY_FULL_VOLTAGE        4.2f
#define BATTERY_EMPTY_VOLTAGE       3.0f
#define NTC_B_COEFFICIENT           3950
#define NTC_NOMINAL_R               10000   // 10kΩ
#define NTC_NOMINAL_TEMP            25.0f   // °C
#define NTC_SERIES_RESISTOR         10000   // 직렬 저항 10kΩ  ← [추가]

// ===== 피부 접촉 감지 (TTP223 터치 센서) =====  ← [변경: ADC → GPIO]
#define SKIN_CONTACT_DEBOUNCE_MS    100     // GPIO 상태 변경 디바운스
#define SKIN_CONTACT_ACTIVE_LEVEL   1       // TTP223: HIGH=접촉, LOW=비접촉

// ===== 진동 =====
#define VIBRATION_MIN_DUTY_PCT  30      // 최소 구동 듀티비 (실측 필요)
#define VIBRATION_PWM_FREQ_HZ   20000   // 가청 범위 밖

// ===== LED =====
#define LED_BREATHE_PERIOD_MS   2000
#define LED_BLINK_FAST_MS       200
#define LED_BLINK_SLOW_MS       500
#define LED_FADE_OUT_MS         1000

// ===== BLE =====
#define BLE_DEVICE_NAME         "OpenGlow"
#define BLE_MAX_CONNECTIONS     1

// ===== 이벤트 큐 =====
#define EVENT_QUEUE_SIZE        16

// ===== Watchdog =====
#define WATCHDOG_TIMEOUT_MS     5000

// ===== 디버그 =====
#define LOG_LEVEL               LOG_LEVEL_INFO
// LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_ERROR

// ===== 핀 매핑 (ESP32) =====
// ⚠️ GPIO0은 부트 모드 선택 핀 — 외부 풀업 저항 필수, 부팅 시 LOW면 다운로드 모드 진입
// 프로토타입에서 문제 발생 시 GPIO2 등으로 변경 검토
#define PIN_BTN_POWER           GPIO_NUM_0
#define PIN_BTN_MODE            GPIO_NUM_4
#define PIN_EMS_PWM             GPIO_NUM_25
#define PIN_EMS_ENABLE          GPIO_NUM_26  // 비상 차단용 별도 핀
#define PIN_LED_DATA            GPIO_NUM_18  // WS2812B RMT
#define PIN_VIBRATION_PWM       GPIO_NUM_19
#define PIN_BATTERY_ADC         ADC1_CHANNEL_6  // GPIO34
#define PIN_TEMP_ADC            ADC1_CHANNEL_7  // GPIO35
#define PIN_CHARGE_STAT         GPIO_NUM_27     // 충전 IC STAT 핀
#define PIN_SKIN_CONTACT        GPIO_NUM_32     // TTP223 터치 센서 (디지털 출력)  ← [변경: ADC → GPIO]
```

---

## 6. 변경 이력  ← [추가 섹션]

| 버전 | 날짜 | 변경 내용 |
|------|------|----------|
| v1   | -    | 초기 설계 |
| v2   | -    | 1차 검토: event_queue, button_handler, skin_contact, HAL 레이어, CHARGING 상태 추가 |
|      |      | 2차 검토: ESP32 LEDC 주파수 제한 분석, PID anti-windup, NTC 온도 변환, 메인 루프 호출 순서 명시, MICRO 모드 소프트웨어 토글 결정 |
|      |      | 3차 검토: 모든 열린 질문 결정, 에러 코드 체계, config.h 전체 설정값, 핀 매핑, 디버그 로깅 전략, Phase별 예상 소요 시간, 변경 이력 추가 |
| v2.1 | -    | 재점검: 10개 이슈 수정 |
|      |      | - RUNNING→CHARGING 전이 추가 (충전기 연결 시 출력 즉시 중단) |
|      |      | - CHARGING 상태에서 전원 버튼 무시 정책 확정 |
|      |      | - 물리 버튼 2개 구성 명시 + 모드 버튼 상태별 역할 변경 설명 |
|      |      | - MODE_SELECT에서 모드 순환 순서 명시 (PULSE→MICRO→EMS→THERMAL) |
|      |      | - event_queue 안전 이벤트 보호 (오버플로 시 비안전 이벤트만 드롭) |
|      |      | - 8분 경고를 EVENT_SAFETY_AUTO_WARNING 이벤트로 변경 (FSM 경유 원칙 유지) |
|      |      | - config.h에 NTC_SERIES_RESISTOR, 피부 접촉 임계값 추가 |
|      |      | - BLE Write 거부 시 Notify 재전송 메커니즘 정의 |
| v2.2 | -    | 최종 점검: 3개 이슈 수정 |
|      |      | - button_handler 설명과 Q3/상태 전이 간 모순 해소 (세기 변경 1차부터 허용으로 통일) |
|      |      | - FSM 검토 포인트의 모드 변경 Phase 참조 수정 (Phase 5 → Phase 6 [23]) |
|      |      | - 충전 이벤트(EVENT_CHARGE_*) event_type_t에 추가 + battery_monitor에서 push 로직 명시 |
| v2.3 | -    | 피부 접촉 감지 방식 변경: ADC 임피던스 → TTP223 GPIO 디지털 입력 |
|      |      | - PIN_SKIN_CONTACT_ADC → PIN_SKIN_CONTACT (GPIO_NUM_32) |
|      |      | - ADC 임계값(THRESHOLD_LOW/HIGH) 제거, ACTIVE_LEVEL 추가 |
|      |      | - skin_contact_is_active() 인터페이스 유지 (향후 ADC 교체 가능) |
