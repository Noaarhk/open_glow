# Bug Log

개발 과정에서 발견한 버그, 원인 분석, 해결 방법을 기록합니다.

---

## BUG-001: ESP-IDF install.sh 가상환경 충돌

- **발견 시점**: Phase 1 — ESP-IDF 설치
- **증상**: `./install.sh esp32` 실행 시 `"ERROR: This script was called from a virtual environment"` 에러
- **원인**: 프로젝트의 `.venv`가 활성화된 상태에서 ESP-IDF가 자체 가상환경을 생성하려다 충돌
- **해결**: PATH에서 `.venv` 경로를 제거하여 가상환경 비활성화 후 재실행
- **교훈**: ESP-IDF 도구 설치 시 다른 Python 가상환경이 비활성화 상태인지 확인

---

## BUG-002: cmake 미설치

- **발견 시점**: Phase 1 — 첫 빌드 시도
- **증상**: `idf.py build` 실행 시 cmake not found
- **원인**: macOS에 cmake가 설치되어 있지 않음
- **해결**: `brew install cmake` (cmake 4.3.0 설치)
- **교훈**: ESP-IDF는 cmake를 별도로 설치하지 않음, 시스템에 미리 설치 필요

---

## BUG-003: PID_TRANSITION_TIME_MS 누락

- **발견 시점**: Phase 1 — config.h 검증 단계
- **증상**: 빌드 에러는 없었지만, 설계문서(IMPLEMENTATION_PLAN.md)와 비교 시 config.h에 정의가 빠져 있음
- **원인**: config.h 작성 시 설계문서의 PID 섹션을 놓침
- **해결**: `#define PID_TRANSITION_TIME_MS 300` 추가
- **교훈**: 코드 작성 후 반드시 설계문서와 1:1 대조 검증 수행 → dev_workflow_rules 규칙으로 등록됨

---

## BUG-004: event_queue.c portMUX_TYPE 매크로 확장 에러

- **발견 시점**: Phase 1 — event_queue/button_handler/device_fsm 빌드
- **증상**: `macro "portENTER_CRITICAL" passed 2 arguments, but takes just 1` 컴파일 에러
- **원인**: `taskENTER_CRITICAL(&(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED)` — 인라인 컴파운드 리터럴이 매크로 전처리 시 쉼표 때문에 인자 2개로 파싱됨
- **해결**: `static portMUX_TYPE queue_mux = portMUX_INITIALIZER_UNLOCKED;` 정적 변수 선언 후 `taskENTER_CRITICAL(&queue_mux)` 로 변경
- **교훈**: ESP32의 critical section 매크로에는 반드시 별도 선언된 portMUX_TYPE 변수의 포인터를 전달해야 함. 인라인 초기화 불가.

---

## BUG-005: 버튼 디바운스 타이밍 데드존 (SHORT 2030ms 오판)

- **발견 시점**: Phase 1 — 하드웨어 테스트 (ESP32 실행)
- **증상**: 버튼을 ~2초 눌렀는데 `SHORT (2030ms)`로 판정됨. LONG이어야 함.
- **원인**: 버튼 해제 시에도 50ms 디바운싱이 필요한데, 디바운싱 구간에서 `return`으로 빠져나가면서 LONG 체크가 스킵됨.
  ```
  ~1980ms: 사용자 손 뗌 (물리적 해제)
  1980~2030ms: 해제 디바운싱 중 → return으로 LONG 체크 건너뜀
  2030ms: 해제 확정 → long_fired=false → SHORT(2030ms) 오판
  ```
- **해결**: 디바운싱 중이라도 `current_state`가 눌린 상태면 홀드 시간 체크를 계속 수행하도록 수정
  ```c
  if (btn->debounce_counter < DEBOUNCE_COUNT) {
      if (btn->current_state) {  // 이미 눌린 상태면 홀드 체크 계속
          uint32_t held_ms = get_time_ms() - btn->press_start_ms;
          // VLONG, LONG 체크 로직
      }
      return;
  }
  ```
- **교훈**: 디바운싱은 누름/해제 양쪽에 적용된다. 해제 디바운싱 구간에서도 홀드 타이머 관련 로직은 계속 실행되어야 한다. 실제 하드웨어 테스트 없이는 발견하기 어려운 타이밍 버그.
- **상태**: 수정 완료, 재플래시 후 검증 필요

---

## BUG-006: 진동 모터 PWM 20kHz + 13bit 해상도 불가

- **발견 시점**: Phase 2 — 하드웨어 테스트 (flash monitor)
- **증상**: `E (384) ledc: requested frequency 20000 and duty resolution 13 can not be achieved` 에러. 진동 모터 PWM 채널 설정 실패.
- **원인**: ESP32 LEDC 타이머 클럭 80MHz에서 20kHz × 8192(13bit) = 약 164MHz가 필요하여 초과. vibration_controller.c가 EMS와 동일한 `EMS_PWM_RESOLUTION_BITS`(13bit)를 사용한 것이 원인.
- **해결**: `VIBRATION_PWM_RESOLUTION_BITS` 10bit(1024 단계) 상수를 별도 정의하고, vibration_controller.c에서 이를 사용하도록 변경. 80MHz / 20kHz = 4000 > 1024(10bit)이므로 정상 동작.
- **교훈**: PWM 주파수와 해상도는 트레이드오프 관계. `APB_CLK / (freq × 2^resolution) ≥ 1`이어야 함. 서로 다른 주파수를 사용하는 모듈은 해상도 상수를 공유하면 안 됨.

---

## BUG-007: FSM 에러 코드 오할당 (BATTERY_CRITICAL → ERR_TEMP_CRITICAL)

- **발견 시점**: Phase 2 완료 후 — 설계문서 대조 검증
- **증상**: `EVENT_SAFETY_BATTERY_CRITICAL` 이벤트가 발생해도 `ERR_BATTERY_CRITICAL`이 아닌 `ERR_TEMP_CRITICAL`이 `error_code`에 저장됨
- **원인**: `handle_running()`에서 3가지 안전 이벤트를 하나의 case 블록으로 묶고, 삼항 연산자로 `TEMP_WARNING`만 구분:
  ```c
  // 수정 전 (잘못됨)
  ctx.error_code = (evt->type == EVENT_SAFETY_TEMP_WARNING) ?
                   ERR_TEMP_WARNING : ERR_TEMP_CRITICAL;
  ```
  `EVENT_SAFETY_BATTERY_CRITICAL`도 else 분기로 빠지면서 `ERR_TEMP_CRITICAL` 할당
- **해결**: 3가지 이벤트를 각각 별도 case로 분리하여 올바른 에러 코드 할당:
  ```c
  case EVENT_SAFETY_TEMP_WARNING:
      ctx.error_code = ERR_TEMP_WARNING;  break;
  case EVENT_SAFETY_TEMP_CRITICAL:
      ctx.error_code = ERR_TEMP_CRITICAL;  break;
  case EVENT_SAFETY_BATTERY_CRITICAL:
      ctx.error_code = ERR_BATTERY_CRITICAL;  break;
  ```
- **교훈**: 여러 case를 fall-through로 묶을 때, 이벤트별로 다른 처리가 필요하면 반드시 분리해야 함. 특히 에러/안전 관련 코드는 각 케이스를 명시적으로 처리하는 것이 안전.

---

## BUG-008: 전원 ON 진동 피드백 누락

- **발견 시점**: Phase 2 완료 후 — 설계문서 대조 검증
- **증상**: 전원 버튼 길게 눌러 OFF→IDLE 전이 시 진동 피드백 없음. 설계문서에는 "전원 ON: 중간 세기 진동 1회 (200ms)" 명시
- **원인**: `fsm_transition()`의 `STATE_IDLE` on_enter 블록에 `vibration_pulse()` 호출이 빠져 있음
- **해결**: `STATE_IDLE` on_enter에서 `previous_state == STATE_OFF`일 때만 `vibration_pulse(200, 0, 1)` 호출 추가. 다른 상태에서 IDLE 복귀 시에는 진동 불필요.
- **교훈**: 출력 모듈의 "사용 시나리오" 목록을 FSM 전이 코드와 1:1 대조해야 누락을 방지할 수 있음.

---

## BUG-009: PAUSE_RESUME_MS (3초 재접촉 제한) 미적용

- **발견 시점**: Phase 2 완료 후 — 설계문서 대조 검증
- **증상**: PAUSED 상태에서 10초 이내 언제든 피부 재접촉하면 RUNNING으로 복귀. 설계문서는 "3초 이내 재접촉 시에만 RUNNING 복귀" 명시.
- **원인**: `handle_paused()`에서 `EVENT_SKIN_CONTACT_ON` 시 경과 시간 확인 없이 무조건 `STATE_RUNNING` 전이
- **해결**: `get_time_ms() - ctx.state_enter_time_ms < PAUSE_RESUME_MS` 조건 추가. 3초 이내 → RUNNING, 3초 초과 → IDLE.
  ```c
  if (get_time_ms() - ctx.state_enter_time_ms < PAUSE_RESUME_MS) {
      fsm_transition(STATE_RUNNING);
  } else {
      fsm_transition(STATE_IDLE);
  }
  ```
- **교훈**: config.h에 정의된 상수가 실제 코드에서 사용되는지 grep으로 확인하는 습관 필요. 미사용 상수는 구현 누락의 신호.

---

## BUG-010: 브레드보드 쇼트로 WS2812B LED 및 IRLZ44N MOSFET 손상

- **발견 시점**: Phase 2 — 진동 모터 하드웨어 디버깅 중
- **증상**: 모터 직접 테스트 중 타는 냄새 + ESP32 전원 꺼짐. 이후 WS2812B LED 점등 불가, MOSFET에 3.3V 연결 시에도 타는 냄새 발생.
- **원인**: 모터 테스트를 위해 5V와 GND 점퍼를 브레드보드에 꽂을 때 같은 행(a~e 또는 f~j)에 꽂아서 쇼트 발생. 과전류가 WS2812B와 MOSFET을 손상.
- **해결**: WS2812B LED와 IRLZ44N MOSFET 교체 필요.
- **교훈**:
  1. 브레드보드에서 전원(+)과 GND(-)를 같은 행에 꽂으면 내부적으로 직결되어 쇼트
  2. 전원 관련 작업 시 꽂기 전에 "이 두 점이 같은 행인가?" 반드시 확인
  3. 모터 테스트는 ESP32 전원 핀 대신 별도 전원(건전지 등)을 사용하는 것이 안전
  4. 쇼트 발생 후 주변 부품(MOSFET, LED 등)도 손상될 수 있으므로 개별 검증 필요
- **손상 부품**: WS2812B LED 1개, IRLZ44N MOSFET 1개
- **정상 부품**: ESP32 보드, 코인 진동모터, 버튼 2개

---

## BUG-011: OFF→IDLE 전이 시 Brownout 리셋

- **발견 시점**: Phase 2 — 전원 ON 진동 피드백 테스트
- **증상**: 전원 버튼 길게 누름 → OFF→IDLE 전이 → `BOD: Brownout detector was triggered` → ESP32 리셋 (재부팅)
- **원인**: `vibration_init()`에서 초기 듀티를 테스트용 **90%**로 설정해둔 상태(`pct_to_duty(90)`)에서, OFF→IDLE 전이 시 `vibration_pulse(200, 0, 1)`이 호출되어 90% 듀티로 모터가 시작됨. LED(WS2812B) + 진동 모터(90% 듀티)의 동시 구동으로 순간 전류가 급증, USB 전원으로 감당하지 못해 전압 강하 발생. Brownout 감지 레벨이 LVL_SEL_0 (가장 민감한 ~2.43V)으로 설정되어 있어 쉽게 트리거됨.
- **해결**: `vibration_init()`의 초기 듀티를 `pct_to_duty(90)` → `pct_to_duty(duty_table[0])` (30%, 세기 1)로 변경
- **교훈**:
  1. 테스트용 임시 값(TODO 주석)은 반드시 정상 값으로 원복해야 함. TODO를 방치하면 실 동작에서 문제 발생.
  2. 여러 출력 모듈이 동시에 켜질 때의 전류 합산을 고려해야 함 (특히 USB 전원 환경).
  3. Brownout 감지 레벨이 민감할수록 개발 중 허위 트리거가 발생할 수 있으므로, 프로토타입 단계에서는 레벨 조정도 고려.

---

## BUG-012: MOSFET 과열 — 진동 모터 배선 오류 (2차)

- **발견 시점**: Phase 2 — 진동 모터 MOSFET 교체 후 재테스트
- **증상**: 진동 펄스 시도 시 모터 무반응 + brownout/리셋 반복. 100% 듀티 강제 테스트 시 MOSFET 극심한 과열 + 타는 냄새.
- **원인 (복합)**:
  1. **모터 전원이 3.3V에 연결됨**: 모터를 ESP32의 3.3V 핀(LDO 출력)에 연결하여, 모터 전류가 ESP32 전원을 직접 끌어내림 → brownout/리셋
  2. **모터 +/-를 하나의 점퍼선으로 연결하여 쇼트**: 모터의 빨강(+)과 파랑(-)을 하나의 점퍼선 양끝에 연결 → 점퍼선이 모터를 우회하는 직통 경로가 됨 (저항 ≈ 0Ω) → 5V → 점퍼선(쇼트) → MOSFET Drain → Source → GND = 과전류 → ESP32 보드 손상
     - 모터의 두 리드선은 **반드시 각각 별도의 점퍼선**으로 연결해야 함
     - 하나의 점퍼선에 +/-를 함께 꽂으면 모터 내부를 거치지 않고 직접 단락됨
- **해결**:
  1. 모터 전원을 3.3V → 5V(VIN)로 변경
  2. 모터 (+), (-) 리드선을 각각 별도 점퍼선으로 연결 → **정상 동작 확인**
  3. ESP32 보드 교체 (과전류로 손상, MOSFET은 정상)
  4. 재배선 시 올바른 회로 확인 필수:
     ```
     5V ── 모터(+) ── 모터(-) ── MOSFET Drain
     GPIO19 ── MOSFET Gate
     GND ── MOSFET Source
     ```
     IRLZ44N 핀배치 (전면 글씨 방향, 왼→오): Gate, Drain, Source
- **교훈**:
  1. 모터/솔레노이드 등 전류 소모가 큰 부하는 반드시 5V(USB 직통)에 연결. 3.3V(LDO)는 ESP32 전용.
  2. **모터 리드선은 반드시 각각 별도 점퍼선으로 연결**. 하나의 점퍼선에 +/-를 함께 꽂으면 모터를 우회하는 쇼트 회로가 됨.
  3. MOSFET 배선 시 반드시 데이터시트로 핀배치 확인 후 연결. "감으로" 꽂지 말 것.
  4. 전원 인가 전에 멀티미터로 쇼트 여부를 먼저 확인하면 부품 손상을 예방할 수 있음.
- **손상 부품**: ESP32 DevKitC V4 보드 1개 (MOSFET은 정상)

---

## BUG-013: ESP32 보드 손상 — MOSFET 과전류로 USB-시리얼 칩 손상 추정

- **발견 시점**: Phase 2 — BUG-012 이후 플래시 시도
- **증상**: ESP32 보드 LED 계속 깜빡임 (정상은 점등 고정). USB에서 시리얼 포트(`/dev/cu.usbserial-*`) 생성 안 됨. `system_profiler`에서 CP2102N 칩 자체가 인식 안 됨. 케이블은 다른 기기에서 정상 동작 확인.
- **원인**: BUG-012의 모터 점퍼선 쇼트로 인한 과전류가 USB를 통해 ESP32 보드로 역류. 보드의 CP2102N USB-시리얼 칩 또는 LDO 레귤레이터가 손상.
- **해결**: ESP32 DevKitC V4 보드 교체. MOSFET은 정상이므로 그대로 사용.
- **교훈**:
  1. 과전류가 발생할 수 있는 외부 회로(모터, MOSFET 등)는 반드시 **별도 전원**(건전지, 외부 어댑터)을 사용하거나, ESP32 보드의 5V 핀에 **퓨즈**를 달아 보호해야 함.
  2. MOSFET이 뜨거워지면 **즉시** 전원을 차단해야 함. 지체할수록 피해가 상류(ESP32 보드)로 확대됨.
  3. 프로토타입 단계에서 여분의 보드를 준비하는 것이 좋음.
- **손상 부품**: ESP32 DevKitC V4 보드 1개 (총 누적: WS2812B 1개, IRLZ44N 1개, ESP32 보드 1개)

---

## BUG-014: 센서 미연결 시 비상 차단 오발동 (온도 93.8°C / 배터리 0%)

- **발견 시점**: Phase 3 — 센서 모듈 통합 후 첫 flash monitor
- **증상**: 부팅 5초 후 `CRITICAL: Temp 93.8°C >= 50°C → EMERGENCY SHUTDOWN` 또는 `CRITICAL: Battery 0% <= 5% → EMERGENCY SHUTDOWN` 발생. GPIO34(배터리), GPIO35(온도) 미연결 상태.
- **원인**:
  1. **온도**: GPIO35 플로팅 → ADC 노이즈 값이 낮은 저항으로 해석 → Steinhart-Hart 변환에서 93.8°C 산출 → 비상 차단
  2. **배터리**: GPIO34 플로팅 → ADC 노이즈 값이 낮은 전압으로 변환 → 0% → 비상 차단
  3. **유효성 검사 부실**: 기존 코드는 raw==0 또는 raw==4095만 센서 고장으로 판단. 플로팅 핀의 중간 노이즈 값은 통과.
  4. **유효성 통합**: `battery_is_valid()`가 전압/온도를 하나로 묶어 판단. 하나만 이상해도 구분 불가.
- **해결**:
  1. `battery_is_valid()` → `battery_is_voltage_valid()` + `battery_is_temp_valid()`로 분리
  2. ADC raw 체크 + **변환값 범위 체크** 이중 검증:
     - 온도: -20~80°C 밖이면 invalid (센서 미연결/고장)
     - 전압: 2.5~4.5V 밖이면 invalid (분압 회로 미연결)
  3. `safety_manager.c`에서 센서별 유효성 확인 후 체크:
     - `check_temperature()`: `battery_is_temp_valid()` false면 스킵
     - `check_battery()`: `battery_is_voltage_valid()` false면 스킵
  4. 초기값을 `valid=true` → `valid=false`로 변경 (첫 ADC 읽기 전까지 미확정)
- **교훈**:
  1. 플로팅 GPIO의 ADC 값은 0이나 4095가 아닌 랜덤 노이즈일 수 있음. raw 체크만으로는 미연결 감지 불가.
  2. **물리적 범위 체크**가 센서 유효성 판단의 핵심. ADC raw → 물리량 변환 후 현실적 범위 내인지 확인.
  3. 안전 모듈은 센서 데이터를 "항상 믿을 수 있다"고 가정하면 안 됨. 유효성이 보장된 데이터만 사용해야 함.

---

## BUG-015: 센서 감지 성공 후 첫 ADC 읽기 전 고장 오판

- **발견 시점**: Phase 3 — 3단계 센서 안전 정책 구현 후 NTC 연결 테스트
- **증상**: NTC가 정상 연결되어 있는데 부팅 직후 `Temp sensor FAULT (was connected, now invalid) → EMERGENCY SHUTDOWN` 발생
- **원인**: `battery_init()`에서 센서를 `connected=true`로 감지하지만, `temp_valid`는 `false`로 초기화. `battery_update()`의 첫 ADC 읽기는 5초 간격 체크(`BATTERY_UPDATE_INTERVAL_MS`) 때문에 스킵됨. 그 사이 `safety_update()` → `check_temperature()`가 호출되어 `connected=true, valid=false` → "고장" → 비상 차단.
  ```
  battery_init():  connected=true, valid=false (초기값)
  main loop 1:     battery_update() → 5초 안 됐으니 ADC 안 읽음 → valid=false 유지
                   safety_update() → connected=true + valid=false → 고장 판정!
  ```
- **해결**: `battery_init()`에서 센서 연결 감지 성공 시 `valid`도 `true`로 세팅. 초기화 시 이미 유효한 값을 여러 번 읽어서 연결을 확인했으므로, 첫 정기 ADC 읽기 전까지는 "유효"로 간주해도 안전.
- **교훈**: 새로운 안전 메커니즘을 추가할 때, 기존 타이밍 흐름(5초 간격 ADC)과의 상호작용을 반드시 검토. "초기화 시 감지"와 "런타임 갱신" 사이의 공백 구간에서 오판이 발생할 수 있음.

---

## BUG-016: ESP_LOGD가 컴파일에서 제외되어 DEBUG 로그 미출력

- **발견 시점**: Phase 3 — NTC 온도 로그 확인 시도
- **증상**: `esp_log_level_set("BAT", ESP_LOG_DEBUG)` 설정 후 `flash monitor`해도 BAT 모듈의 DEBUG 로그가 출력되지 않음
- **원인**: ESP-IDF의 `sdkconfig`에서 `CONFIG_LOG_MAXIMUM_LEVEL=3` (INFO)으로 설정되어 있어, `ESP_LOGD()` 호출이 **컴파일 시점에** 제거됨. 런타임에 레벨을 바꿔도 바이너리에 코드 자체가 없으므로 효과 없음. 추가로 우리 `debug_log.h`의 `LOG_DEBUG` 매크로도 `LOG_LEVEL`에 의해 컴파일 시점 게이팅되는 이중 구조.
  ```
  레이어 1 (우리 코드): LOG_LEVEL=INFO → LOG_DEBUG(...) = ((void)0)
  레이어 2 (ESP-IDF):   CONFIG_LOG_MAXIMUM_LEVEL=3 → ESP_LOGD() 제거
  → 두 레이어 모두 풀어야 DEBUG 출력 가능
  ```
- **해결**:
  1. `openglow_config.h`: `LOG_LEVEL`을 `LOG_LEVEL_DEBUG`로 변경 (우리 매크로 게이트 해제)
  2. `sdkconfig`: `CONFIG_LOG_DEFAULT_LEVEL=4`, `CONFIG_LOG_MAXIMUM_LEVEL=4`로 변경 (ESP-IDF 게이트 해제)
  3. `main.c`: `esp_log_level_set("*", ESP_LOG_INFO)` + `esp_log_level_set("BAT", ESP_LOG_DEBUG)`로 런타임 제어 — 전체는 INFO, 보고 싶은 모듈만 DEBUG
- **교훈**: ESP-IDF 로그 시스템은 컴파일 타임(`CONFIG_LOG_MAXIMUM_LEVEL`)과 런타임(`esp_log_level_set`)의 2단계 필터링. 런타임 설정만으로는 컴파일 타임에 제거된 로그를 복구할 수 없음. 개발 단계에서는 `MAXIMUM_LEVEL`을 DEBUG로 열어두고 런타임에서 모듈별로 제어하는 것이 유연함.

---

## BUG-017: button_handler.c가 HAL을 우회하여 ESP-IDF GPIO API 직접 호출

- **발견 시점**: Phase 4 완료 후 — 면접 대비 코드 리뷰 중
- **증상**: `button_handler.c`가 `#include "driver/gpio.h"`로 ESP-IDF API(`gpio_config()`, `gpio_get_level()`)를 직접 호출. 다른 모든 모듈(`ems_controller`, `skin_contact`, `safety_manager` 등)은 `hal/hal_gpio.h`를 경유하는데 버튼만 예외.
- **원인**: Phase 1 개발 시 버튼 핸들러를 HAL보다 먼저 작성하면서 직접 호출로 구현. 이후 HAL 완성 후 버튼 핸들러를 HAL 경유로 전환하지 않은 일관성 누락.
- **영향**: 기능상 문제 없음 (HAL 함수가 동일한 ESP-IDF API를 래핑). 단, 아키텍처 원칙("모든 하드웨어 접근은 HAL을 통해서만") 위반이며, PC 단위 테스트 시 `gpio_get_level()` mock 불가로 테스트 커버리지 저하.
- **해결**:
  1. `#include "driver/gpio.h"` → `#include "hal/hal_gpio.h"`
  2. `gpio_config()` 8줄 → `hal_gpio_set_input(pin, true)` 1줄 (동일 설정 생성 확인 완료)
  3. `gpio_get_level(btn->pin)` → `hal_gpio_read(btn->pin)`
  4. 빌드 검증: 0 error, 0 warning
- **교훈**: 추상화 레이어를 설계한 후에는 모든 모듈이 실제로 해당 레이어를 사용하는지 점검 필요. 특히 초기 개발 단계에서 먼저 작성된 코드는 나중에 완성된 인프라를 반영하지 못할 수 있음. 코드 리뷰나 `grep`으로 직접 의존(예: `#include "driver/"`)을 정기적으로 검사하면 예방 가능.

---

## BUG-018: safety_get_output_limit() 미연결 — 안전 출력 제한 미작동

- **발견 시점**: Phase 4 완료 후 — 면접 대비 3회 코드 검증 중
- **증상**: `safety_manager`가 온도/배터리 상태에 따라 `ctx.output_limit`를 계산하지만 (예: 42°C → 0.8, 배터리 10% → 0.7), 이 값이 EMS 출력에 전혀 반영되지 않음. Level 1 안전 출력 제한(점진적 감소)이 완전히 무효.
- **원인**: `safety_get_output_limit()`과 `ems_set_output_limit()` 두 함수가 각각 선언/정의되어 있지만, 이 둘을 연결하는 호출 코드가 `main.c`, `device_fsm.c` 등 어디에도 없음. `safety_manager.c:16`의 주석에 의도된 흐름(`fsm_update() → safety_get_output_limit() → ems_set_output_limit()`)이 적혀 있으나 구현되지 않음.
  ```
  safety_manager.c:264  — float safety_get_output_limit(void) { return ctx.output_limit; }  ← 정의됨
  ems_controller.c:149  — void ems_set_output_limit(float limit) { ... }                    ← 정의됨
  main.c / device_fsm.c — 호출 없음                                                          ← 누락!
  ```
- **영향**: Level 2 비상 차단(`safety_emergency_shutdown()` → GPIO26 LOW + EMS 정지)만 작동. Level 1 점진적 출력 제한은 완전히 비활성 상태:
  - 온도 40~50°C 구간에서 출력이 줄어들지 않음 (100% 유지)
  - 배터리 10% 이하에서도 출력이 70%로 제한되지 않음
  - 50°C 또는 5% 도달 시 비상 차단만 발동
- **해결**: `main.c` 메인 루프에서 `fsm_update()` 이후, `ems_update()` 이전에 호출 추가:
  ```c
  fsm_update();
  ems_set_output_limit(safety_get_output_limit());  /* ← 추가 */
  ems_update();
  ```
- **교훈**:
  1. 함수를 선언/정의하는 것과 실제로 호출하는 것은 별개. `grep`으로 "정의는 있지만 호출이 없는 함수"를 정기적으로 검사해야 함.
  2. 안전 관련 코드는 "의도대로 연결되었는지" 통합 테스트가 필수. 단위 테스트에서 각 함수가 올바른 값을 반환해도, 호출 체인이 끊어져 있으면 무의미.
  3. 주석에 적힌 설계 의도와 실제 코드의 일치 여부를 검증하는 습관이 중요.
