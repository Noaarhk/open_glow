# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# OpenGlow - ESP32 뷰티 디바이스 펌웨어

## 프로젝트 개요

가정용 뷰티 디바이스 프로토타입 펌웨어.
ESP32 기반으로 EMS/MICRO/THERMAL/PULSE 4가지 모드의 PWM 출력을 제어하고,
BLE로 스마트폰 앱과 통신하는 임베디드 시스템.

- **MCU**: ESP32 DevKitC V4 (ESP-WROOM-32E)
- **프레임워크**: ESP-IDF v5.4
- **언어**: C
- **개발 환경**: macOS + VS Code + ESP-IDF Extension

## 프로젝트 구조

```
~/apr_prep/
├── CLAUDE.md                    # 이 파일
├── docs/
│   ├── SETUP_GUIDE.md           # ESP-IDF 설치 및 개발환경 세팅 가이드
│   └── IMPLEMENTATION_PLAN.md   # 상세 구현계획서 (v2)
├── firmware/                    # ESP-IDF 프로젝트 (idf.py create-project로 생성)
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.c               # 엔트리포인트 (초기화 + 메인 루프)
│       ├── openglow_config.h     # 전체 설정값 (핀 매핑, 타이머, PID 등)
│       ├── event_queue.h/c       # 링 버퍼 기반 이벤트 큐
│       ├── device_fsm.h/c        # 상태 머신 (중앙 컨트롤러)
│       ├── button_handler.h/c    # 버튼 디바운싱 + 길게/짧게 판별
│       ├── skin_contact.h/c      # 피부 접촉 감지
│       ├── ems_controller.h/c    # EMS PWM 펄스 생성
│       ├── pid_controller.h/c    # PID 제어 유틸리티
│       ├── led_controller.h/c    # WS2812B 네오픽셀 RGB LED
│       ├── vibration_controller.h/c # Coin 진동 모터 PWM
│       ├── safety_manager.h/c    # 안전 제어 (이중 안전 구조)
│       ├── battery_monitor.h/c   # ADC 배터리/온도 측정
│       ├── ble_service.h/c       # BLE GATT 서버
│       ├── debug_log.h/c         # UART 시리얼 로깅
│       └── hal/                  # Hardware Abstraction Layer
│           ├── hal_gpio.h/c
│           ├── hal_adc.h/c
│           ├── hal_pwm.h/c
│           ├── hal_timer.h/c
│           └── hal_nvs.h/c
└── test/                        # PC 단위 테스트 (HAL mock 사용)
    ├── CMakeLists.txt
    └── test_fsm.c
```

## 아키텍처 핵심 원칙

1. **FSM 중앙 조율**: 모든 모듈은 FSM(device_fsm)을 통해 조율. 모듈 간 직접 호출 최소화.
2. **이벤트 드리븐**: 모듈 간 통신은 event_queue를 통해 수행. BLE 콜백도 event_queue로 동기화.
3. **이중 안전**: safety_manager는 정상 시 FSM 경유, 비상 시(50도↑, 배터리 5%↓) FSM 우회하여 GPIO 직접 차단.
4. **HAL 추상화**: 모든 하드웨어 접근은 hal/ 레이어를 통해서만. PC mock으로 단위 테스트 가능.

## FSM 상태

```
OFF → IDLE → MODE_SELECT → RUNNING → PAUSED
                                    → CHARGING
                                    → ERROR → SHUTDOWN → OFF
```

- OFF: deep sleep
- IDLE: 대기 (3분 무조작 시 자동 슬립)
- MODE_SELECT: 모드 순환 (PULSE→MICRO→EMS→THERMAL)
- RUNNING: 출력 동작 중 (세기 1~5 조절 가능)
- PAUSED: 피부 접촉 해제 (10초 후 IDLE)
- CHARGING: 충전 중 (출력 불가)
- ERROR: 과열/저전압 (복구 가능 시 IDLE, 불가 시 SHUTDOWN)
- SHUTDOWN: 안전 차단 후 OFF

## 물리 버튼 매핑 (2개)

```
[전원 버튼]
  OFF       → 길게(2초) → IDLE
  IDLE      → 길게(3초) → SHUTDOWN
  MODE_SELECT → 짧게 → RUNNING 시작
  PAUSED    → 짧게 → IDLE 복귀
  
[모드 버튼]
  IDLE        → 짧게 → MODE_SELECT 진입
  MODE_SELECT → 짧게 → 다음 모드 순환
  RUNNING     → 짧게 → 세기 순환 (1→2→3→4→5→1)
```

## 모드별 PWM 파라미터

| 모드 | 주파수 | 듀티비 범위 | ESP32 구현 |
|------|--------|-----------|----------|
| PULSE | 1kHz | 10~50% | LEDC (13bit) |
| MICRO (마이크로커런트) | 10Hz | 5~30% | SW GPIO 토글 |
| EMS (근육) | 3kHz | 15~60% | LEDC (13bit) |
| THERMAL | 5kHz | 10~45% | LEDC (13bit) |

## ESP32 핀 매핑

```
GPIO0  → 전원 버튼 (⚠️ 부트 핀, 외부 풀업 필수)
GPIO4  → 모드 버튼
GPIO25 → EMS PWM 출력
GPIO26 → EMS ENABLE (비상 차단용)
GPIO18 → WS2812B LED 데이터 (RMT)
GPIO19 → 진동 모터 PWM
GPIO27 → 충전 IC STAT 핀
GPIO32 → 피부 접촉 ADC (ADC1_CH4)
GPIO34 → 배터리 전압 ADC (ADC1_CH6)
GPIO35 → 온도 센서 ADC (ADC1_CH7)
```

## 메인 루프 구조 (10ms / 100Hz)

```
입력 수집 → 안전 체크 → FSM 판단 → 출력 반영 → BLE 동기화

while(1) {
    watchdog_feed();
    button_update();         // 디바운싱 → 이벤트 push
    skin_contact_update();   // 접촉 감지 → 이벤트 push
    battery_update();        // ADC (5초 간격) + 충전 GPIO (매 루프)
    safety_update();         // 위험 감지 → 이벤트/비상 차단
    fsm_update();            // 이벤트 소비 → 상태 전이 → 출력 제어
    ems_update();            // PID 업데이트
    led_update();            // LED 패턴
    vibration_update();      // 진동 패턴
    ble_update();            // Notify 전송
    vTaskDelay(10ms);
}
```

## 구현 순서 (Phase별)

### Phase 1: 기반 구조 ← 현재 단계
1. ESP-IDF 프로젝트 생성 + 빌드/플래시 확인
2. openglow_config.h (설정값 + 에러 코드)
3. hal/ 레이어 (GPIO, ADC, PWM, Timer, NVS)
4. event_queue (링 버퍼, ISR-safe)
5. button_handler (디바운싱 + 길게/짧게)
6. device_fsm (상태 머신 뼈대)
7. debug_log (UART 로깅)
8. main.c (초기화 + 메인 루프 + watchdog)

### Phase 2: 출력 모듈
8. ems_controller (PWM, PID 없이 즉시 전환)
9. led_controller (단색 + 모드별 색상)
10. vibration_controller (on/off + 펄스)

### Phase 3: 센서 및 안전
11. skin_contact (접촉 감지)
12. battery_monitor (ADC + 이동평균)
13. safety_manager (자동 꺼짐 + 비상 차단 + 저전압)

### Phase 4: 통신
14. ble_service (GATT 서버)
15. nRF Connect로 Read/Write 테스트
16. NVS 세션 로그

### Phase 5: 고도화
17~22. PID 트랜지션, LED 패턴, BLE Notify, 비선형 배터리 보간 등

### Phase 6: 보너스
23~25. RUNNING 중 모드 전환, OTA, 커스텀 LED 색상

## 현재 상태

**Phase 4 (통신)** — BLE GATT 서버 구현 + nRF Connect 테스트 완료.

### 완료된 Phase
- **Phase 1 (기반 구조)** ✅ — config, HAL, event_queue, button, FSM, debug_log, main.c
- **Phase 2 (출력 모듈)** ✅ — EMS PWM(4모드), LED(WS2812B RMT), 진동(코인 모터)
- **Phase 3 (센서 및 안전)** ✅ — 피부 접촉(TTP223), 배터리/온도(ADC), 안전 관리자(이중 안전 + 3단계 센서 정책)
- **Phase 4 (통신)** ✅ — BLE GATT 서버(9개 Characteristic), Read/Write/Notify 동작 확인

### 하드웨어 테스트 현황
- 전원/모드 버튼 (GPIO0/4) ✅
- WS2812B LED (GPIO18) ✅
- 진동 모터 + IRLZ44N (GPIO19) ✅
- TTP223 터치 센서 (GPIO32) ✅
- NTC 온도 센서 (GPIO35) ✅
- 배터리 전압 ADC (GPIO34) ✅
- 충전 감지 (GPIO27) ⏭️ (TP4056 CHRG 핀 미노출)
- BLE 연결 (nRF Connect) ✅

### 다음 단계
- Phase 5 (고도화): PID 트랜지션, LED 패턴, 비선형 배터리 보간 등
- NVS 세션 로그 (Phase 4 잔여)

## 빌드 & 플래시

```bash
# ESP-IDF 환경 활성화 (매 터미널 세션마다 필요)
. ~/esp/esp-idf/export.sh
# 또는 alias 설정 시: get_idf

# 빌드
cd ~/apr_prep/firmware
idf.py build

# 플래시 (USB 포트는 실제 연결된 포트로 변경)
idf.py -p /dev/cu.usbserial-* flash

# 시리얼 모니터
idf.py -p /dev/cu.usbserial-* monitor

# 빌드 + 플래시 + 모니터 한번에
idf.py -p /dev/cu.usbserial-* flash monitor

# PC 단위 테스트 (HAL mock 사용, firmware 미작성 시 해당 없음)
cd ~/apr_prep/test
cmake -B build && cmake --build build && ./build/test_runner
```

## 코딩 컨벤션

- 함수명: `모듈명_동작()` (예: `fsm_update()`, `ems_start()`)
- 파일명: 모듈명.h/c (예: `device_fsm.h`, `device_fsm.c`)
- 상수: 대문자 + 언더스코어 (예: `SAFETY_TEMP_CRITICAL_C`)
- 로그: `LOG_INFO()`, `LOG_WARN()`, `LOG_ERROR()`, `LOG_DEBUG()`
- 로그 포맷: `[timestamp] [모듈] [레벨] 메시지`

## 참고 문서

- `docs/IMPLEMENTATION_PLAN.md` — 상세 구현계획서 v2.2 (모듈별 함수 시그니처, 데이터 구조, 검토 포인트, config.h 전체 설정값)
- `docs/SETUP_GUIDE.md` — ESP-IDF 설치 및 개발환경 세팅 가이드 (macOS)
- ESP-IDF 공식 문서: https://docs.espressif.com/projects/esp-idf/en/v5.4/

## 개발 워크플로우 규칙 (필수)

### 1. 단계별 검증
코드를 작성하거나 수정한 후, 다음 단계로 넘어가기 전에 **반드시** 검증:
- **빌드 체크**: `idf.py build` — 0 error, 0 warning 확인
- **설계문서 대조**: `docs/IMPLEMENTATION_PLAN.md`와 코드의 값/시그니처/상태 전이가 일치하는지 확인
- **누락 확인**: 설계문서에 있는데 코드에 빠진 항목이 없는지 체크
- 불일치 발견 시 수정 후 재빌드까지 완료

### 2. 버그 기록
개발 중 버그를 발견하면 `docs/BUG_LOG.md`에 기록:
- 발견 시점, 증상, 원인, 해결 방법, 교훈
- 번호 형식: BUG-001, BUG-002, ...

### 3. Q&A 기록
사용자의 질문과 답변을 `docs/study/QNA_LOG.md`에 기록:
- 질문 맥락, 답변 요약
- 번호 형식: Q-001, Q-002, ...

### 4. 사용자 수준 고려
사용자는 펌웨어 개발 입문자. 새로운 개념이나 작업을 수행할 때는 **왜 이렇게 하는지** 설명을 포함할 것.

## 주의사항

- GPIO0은 ESP32 부트 모드 선택 핀 — 전원 버튼으로 사용 시 외부 풀업 저항 필수
- MICRO 모드(10Hz)는 LEDC 최소 주파수 제한으로 소프트웨어 GPIO 토글로 구현
- BLE 콜백은 ESP-IDF 별도 태스크에서 실행 — FSM 함수 직접 호출 금지, event_queue로만 통신
