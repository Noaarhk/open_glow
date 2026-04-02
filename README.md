# OpenGlow — ESP32 뷰티 디바이스 펌웨어

가정용 뷰티 디바이스 프로토타입 펌웨어.
ESP32 기반으로 EMS/MICRO/THERMAL/PULSE 4가지 모드의 PWM 출력을 제어하고,
BLE로 스마트폰 앱과 통신하는 임베디드 시스템입니다.

## 하드웨어

- **MCU**: ESP32 DevKitC V4 (ESP-WROOM-32E)
- **출력**: EMS 전극 (PWM), WS2812B RGB LED, 코인 진동 모터
- **입력**: 전원/모드 버튼, TTP223 터치 센서, NTC 10K 서미스터
- **통신**: BLE GATT 서버 (nRF Connect 테스트 완료)
- **전원**: 18650 리튬이온 (3.7V 2200mAh) + TP4056 충전 모듈

## 기술 스택

- **프레임워크**: ESP-IDF v5.4
- **언어**: C
- **개발 환경**: macOS + VS Code + ESP-IDF Extension

## 아키텍처

```
┌─────────────────────────────────────────────────┐
│                   main.c (메인 루프 100Hz)         │
│  입력 수집 → 안전 체크 → FSM 판단 → 출력 반영 → BLE │
└──────────────────────┬──────────────────────────┘
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
   ┌──────────┐  ┌──────────┐  ┌──────────┐
   │  button   │  │   skin   │  │ battery  │
   │ _handler  │  │ _contact │  │ _monitor │
   └────┬─────┘  └────┬─────┘  └────┬─────┘
        │              │              │
        ▼              ▼              ▼
   ┌──────────────────────────────────────┐
   │         event_queue (링 버퍼)         │  ← BLE 콜백도 여기로 push
   └──────────────────┬───────────────────┘
                      ▼
   ┌──────────────────────────────────────┐
   │     device_fsm (중앙 상태 머신)        │
   │  이벤트 소비 → 상태 전이 → 출력 명령    │
   └──┬──────────┬──────────┬─────────────┘
      ▼          ▼          ▼
 ┌─────────┐ ┌───────┐ ┌───────────┐
 │  ems    │ │  led  │ │ vibration │
 │_control │ │_control│ │ _control  │
 └────┬────┘ └───┬───┘ └─────┬─────┘
      │          │            │
      ▼          ▼            ▼
   ┌──────────────────────────────────────┐
   │      HAL (Hardware Abstraction)       │
   │   GPIO / ADC / PWM / Timer / NVS      │
   └──────────────────────────────────────┘
```

**핵심 원칙:**
- **FSM 중앙 조율** — 모든 모듈은 FSM을 통해 조율. 모듈 간 직접 호출 최소화.
- **이벤트 드리븐** — 모듈 간 통신은 event_queue를 통해 수행.
- **이중 안전** — safety_manager는 비상 시 FSM 우회하여 GPIO 직접 차단.
- **HAL 추상화** — 하드웨어 접근은 HAL 레이어를 통해서만. PC mock으로 단위 테스트 가능.

## 동작 모드

| 모드 | 주파수 | 듀티비 범위 | 용도 |
|------|--------|------------|------|
| PULSE | 1kHz | 10~50% | 펄스 |
| MICRO | 10Hz | 5~30% | 마이크로커런트 |
| EMS | 3kHz | 15~60% | 근육 자극 |
| THERMAL | 5kHz | 10~45% | 서멀 |

## 프로젝트 구조

```
firmware/main/
├── main.c                  # 엔트리포인트 (초기화 + 메인 루프)
├── openglow_config.h       # 전체 설정값 (핀 매핑, 타이머, PID 등)
├── device_fsm.h/c          # 상태 머신 (중앙 컨트롤러)
├── event_queue.h/c         # 링 버퍼 기반 이벤트 큐
├── button_handler.h/c      # 버튼 디바운싱 + 길게/짧게 판별
├── ems_controller.h/c      # EMS PWM 펄스 생성
├── led_controller.h/c      # WS2812B 네오픽셀 RGB LED
├── vibration_controller.h/c # 코인 진동 모터 PWM
├── skin_contact.h/c        # TTP223 피부 접촉 감지
├── battery_monitor.h/c     # ADC 배터리/온도 측정 + 이동평균 필터
├── safety_manager.h/c      # 이중 안전 (FSM 경유 + GPIO 직접 차단)
├── ble_service.h/c         # BLE GATT 서버 (Bluedroid)
├── debug_log.h             # UART 시리얼 로깅
└── hal/                    # Hardware Abstraction Layer
    ├── hal_gpio.h/c
    ├── hal_adc.h/c
    ├── hal_pwm.h/c
    ├── hal_timer.h/c
    └── hal_nvs.h/c
```

## 빌드 & 실행

```bash
# ESP-IDF 환경 활성화 (매 터미널 세션마다 필요)
. ~/esp/esp-idf/export.sh

# 빌드
cd firmware
idf.py build

# 플래시 + 시리얼 모니터
idf.py -p /dev/cu.usbserial-* flash monitor
```

## 구현 진행 상황

- [x] **Phase 1**: 기반 구조 — config, HAL, event_queue, button, FSM, 메인 루프
- [x] **Phase 2**: 출력 모듈 — EMS PWM(4모드), LED(WS2812B), 진동 모터
- [x] **Phase 3**: 센서 및 안전 — TTP223 터치, 배터리/온도 ADC, 안전 관리자 (3단계 센서 정책)
- [x] **Phase 4**: 통신 — BLE GATT 서버 (9개 Characteristic, Read/Write/Notify)
- [ ] **Phase 5**: 고도화 — PID 트랜지션, LED 패턴, 비선형 배터리 보간
- [ ] **Phase 6**: 보너스 — OTA, 커스텀 LED 등

## 문서

- [CLAUDE.md](CLAUDE.md) — 프로젝트 설계 및 코딩 컨벤션
- [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md) — 상세 구현계획서 (모듈별 함수 시그니처, 데이터 구조)
- [docs/SETUP_GUIDE.md](docs/SETUP_GUIDE.md) — ESP-IDF 설치 및 개발환경 세팅 가이드 (+ 하드웨어 부품/배선표)
- [docs/BUG_LOG.md](docs/BUG_LOG.md) — 버그 기록 (BUG-001 ~ 016)
