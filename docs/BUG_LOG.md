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
