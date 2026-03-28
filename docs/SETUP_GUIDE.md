# ESP-IDF 개발환경 세팅 가이드 (macOS)

## 전제 조건

- macOS (Apple Silicon 또는 Intel)
- VS Code 설치됨
- Git 설치됨 (`git --version`으로 확인)
- Python 3.9+ 설치됨 (`python3 --version`으로 확인)

---

## 1단계: ESP-IDF 설치

### 1.1 ESP-IDF 다운로드

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
```

- `-b v5.4`: 안정 버전 5.4 지정
- `--recursive`: 하위 라이브러리 포함 다운로드
- 소요 시간: 5~10분

### 1.2 ESP-IDF 도구 설치

```bash
cd ~/esp/esp-idf
./install.sh esp32
```

이 스크립트가 설치하는 것:
- **크로스 컴파일러** (xtensa-esp32-elf-gcc): C 코드를 ESP32 기계어로 변환
- **esptool.py**: 펌웨어를 ESP32에 업로드(플래시)하는 도구
- **Python 가상환경**: ESP-IDF 빌드 시스템이 사용하는 Python 패키지들

### 1.3 환경 변수 활성화

```bash
. ~/esp/esp-idf/export.sh
```

- 매 터미널 세션마다 실행해야 함
- `idf.py` 명령어를 사용할 수 있게 PATH에 도구들을 추가
- 편의를 위해 ~/.zshrc에 별칭 추가 권장:

```bash
echo 'alias get_idf=". ~/esp/esp-idf/export.sh"' >> ~/.zshrc
source ~/.zshrc
```

이후 터미널에서 `get_idf`만 입력하면 환경 활성화됨.

### 1.4 설치 확인

```bash
idf.py --version
```

`ESP-IDF v5.4` 비슷한 출력이 나오면 성공.

---

## 2단계: VS Code 확장 설치

### 2.1 ESP-IDF Extension 설치

1. VS Code 열기
2. 확장(Extensions) 탭 (Cmd+Shift+X)
3. "ESP-IDF" 검색
4. **Espressif IDF** 확장 설치 (제작자: Espressif Systems)
5. 설치 후 VS Code 재시작

### 2.2 Extension 설정

1. Cmd+Shift+P → "ESP-IDF: Configure ESP-IDF Extension"
2. "USE EXISTING SETUP" 선택
3. ESP-IDF 경로: `~/esp/esp-idf`
4. 도구 경로: 자동 감지됨 (보통 `~/.espressif`)
5. "Install" 클릭

### 2.3 유용한 VS Code 설정

`.vscode/settings.json`에 추가하면 IntelliSense가 ESP-IDF 헤더를 인식:

```json
{
    "idf.espIdfPath": "~/esp/esp-idf",
    "idf.toolsPath": "~/.espressif",
    "C_Cpp.default.compileCommands": "${workspaceFolder}/firmware/build/compile_commands.json"
}
```

---

## 3단계: 프로젝트 생성 및 첫 빌드

### 3.1 프로젝트 생성

```bash
# ESP-IDF 환경 활성화
get_idf

# 프로젝트 디렉토리로 이동
cd ~/esp_prep

# ESP-IDF 프로젝트 생성
idf.py create-project -p firmware openglow
```

생성되는 구조:
```
firmware/
├── CMakeLists.txt      # 프로젝트 빌드 설정
└── main/
    ├── CMakeLists.txt   # main 컴포넌트 빌드 설정
    └── openglow.c       # 엔트리포인트 (나중에 main.c로 리팩토링)
```

### 3.2 타겟 칩 설정

```bash
cd ~/esp_prep/firmware
idf.py set-target esp32
```

이 명령이 `sdkconfig` 파일을 생성. ESP32 칩에 맞는 기본 설정이 들어감.

### 3.3 첫 빌드

```bash
idf.py build
```

- 첫 빌드는 ESP-IDF 전체 프레임워크를 컴파일하므로 3~5분 소요
- 이후 빌드는 변경된 파일만 컴파일하므로 수 초 내 완료
- `build/` 디렉토리에 바이너리 생성

### 3.4 ESP32 연결 및 포트 확인

1. ESP32 DevKitC를 USB-C 케이블로 Mac에 연결
2. 터미널에서 포트 확인:

```bash
ls /dev/cu.usb*
```

출력 예시: `/dev/cu.usbserial-0001` 또는 `/dev/cu.usbmodem-XXXX`

> **포트가 안 보이는 경우**:
> - ESP32 DevKitC V4는 보통 CP2102 또는 CH340 USB-UART 칩 사용
> - CH340인 경우 macOS 드라이버 설치 필요:
>   https://github.com/WCHSoftware/ch34xser_macos
> - CP2102인 경우 대부분 자동 인식됨

### 3.5 플래시 및 모니터

```bash
# 펌웨어 업로드
idf.py -p /dev/cu.usbserial-0001 flash

# 시리얼 모니터 (Ctrl+] 로 종료)
idf.py -p /dev/cu.usbserial-0001 monitor

# 빌드 + 플래시 + 모니터 한번에
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

기본 프로젝트는 `app_main()`에서 "Hello world!"를 출력.
모니터에서 이 메시지가 보이면 **개발환경 세팅 완료**.

---

## 4단계: Hello World 확인 후 다음 단계

시리얼 모니터에서 아래와 비슷한 출력이 보이면 성공:

```
I (xxx) main: Hello world!
```

이 시점에서 `openglow.c`의 내용을 OpenGlow Phase 1 코드로 교체하기 시작합니다.

---

## 트러블슈팅

### "Permission denied" on flash
```bash
sudo chmod 666 /dev/cu.usbserial-*
```

### "Failed to connect" on flash
1. ESP32의 BOOT 버튼을 누른 상태에서 flash 명령 실행
2. "Connecting..." 메시지 후 BOOT 버튼 해제
3. 일부 보드는 자동 리셋 회로가 없어서 수동 조작 필요

### 빌드 에러: "cmake not found"
```bash
brew install cmake ninja
```

### Python 관련 에러
```bash
# ESP-IDF가 설치한 Python 가상환경 재설치
cd ~/esp/esp-idf
./install.sh esp32
```

### VS Code IntelliSense 에러 (빨간 줄)
```bash
# 빌드를 한 번 실행하면 compile_commands.json이 생성되어 해결됨
cd ~/esp_prep/firmware
idf.py build
```

---

## 자주 쓰는 명령어 요약

| 명령어 | 설명 |
|--------|------|
| `get_idf` | ESP-IDF 환경 활성화 (별칭 설정 후) |
| `idf.py build` | 빌드 |
| `idf.py flash` | 플래시 |
| `idf.py monitor` | 시리얼 모니터 (Ctrl+] 종료) |
| `idf.py flash monitor` | 플래시 + 모니터 |
| `idf.py menuconfig` | SDK 설정 GUI (BLE, WiFi 등 활성화) |
| `idf.py fullclean` | 빌드 캐시 전체 삭제 |
| `idf.py size` | 펌웨어 크기 분석 |

---

## 하드웨어 부품 목록

### 보유 부품

| # | 부품명 | 수량 | 스펙 | 용도 |
|---|--------|------|------|------|
| 1 | ESP32 DevKitC V4 | 1개 | USB-C, ESP-WROOM-32E 모듈 | 메인 컨트롤러 |
| 2 | 브레드보드 830홀 + 전원공급 모듈 + 점퍼선 키트 | 1세트 | — | 프로토타이핑 |
| 3 | WS2812B 네오픽셀 RGB LED 모듈 | 2개 | 5V, 단일 LED | 상태 표시 LED |
| 4 | 택트 스위치 12×12mm | 2개 | 4핀 DIP | 전원/모드 버튼 |
| 5 | Coin 진동 모터 | 1개 | 외경 8mm, 두께 2.7mm | 햅틱 피드백 |
| 6 | IRLZ44N MOSFET | 3개 | N-ch, Logic-level (Vgs(th) ~1-2V) | 모터/EMS 구동 |
| 7 | 1N4001 다이오드 | 20개 | 1A, 50V | 역기전력 보호 |
| 8 | NTC 온도센서 서미스터 | 1개 | 10K, B=3950, 방수형 프로브 | 온도 측정 |
| 9 | TTP223 터치 센서 모듈 | 1개 | 정전용량식, 3핀 (VCC/GND/SIG) | 피부 접촉 감지 |
| 10 | TP4056 충전 모듈 | 1개 | USB-C 입력, 보호회로 내장 | 배터리 충전 |
| 11 | 18650 리튬이온 배터리 | 1개 | 3.7V 2200mAh, 보호회로 | 전원 공급 |
| 12 | 18650 배터리 홀더 | 1개 | 1구, 와이어형 | 배터리 장착 |
| 13 | 10KΩ 저항 (1/4W) | 10개 | — | NTC 분압회로, 풀업/풀다운 |
| 14 | 330Ω 저항 (1/4W) | 10개 | — | 네오픽셀 데이터 신호 안정화 |
| 15 | IRF520 MOSFET 모듈 | 1개 | Vgs(th) ~4V | 테스트용 (3.3V 구동 부적합) |

### 핀 배선표

| GPIO | 부품 | 연결 방법 | Phase |
|------|------|-----------|-------|
| GPIO0 | 전원 버튼 (택트스위치) | 스위치→GND, 외부 10kΩ 풀업→3.3V | 1 ✅ |
| GPIO4 | 모드 버튼 (택트스위치) | 스위치→GND, 내부 풀업 사용 | 1 ✅ |
| GPIO18 | WS2812B LED | 330Ω 직렬→DIN, VCC→3.3V | 1 ✅ |
| GPIO19 | Coin 진동 모터 | IRLZ44N Gate (10kΩ 풀다운), 모터→Drain, 1N4001 역병렬 | 2 |
| GPIO25 | EMS PWM 출력 | IRLZ44N Gate (10kΩ 풀다운), LED→Drain (시각화) | 2 |
| GPIO26 | EMS ENABLE | IRLZ44N Gate (10kΩ 풀다운), LED→Drain (시각화) | 2 |
| GPIO32 | TTP223 터치 센서 | SIG→GPIO32, VCC→3.3V, GND→GND | 3 |
| GPIO34 | 배터리 전압 ADC | 10kΩ+10kΩ 분압 (TP4056 OUT+ → GPIO34 → GND) | 3 |
| GPIO35 | NTC 온도 센서 ADC | 10kΩ 고정 + NTC 10K 분압 (3.3V → GPIO35 → GND) | 3 |
| GPIO27 | 충전 상태 감지 | TP4056 CHRG 핀→GPIO27, 10kΩ 풀업→3.3V | 3 |

### 회로 상세

**진동 모터 (GPIO19)**
```
GPIO19 ─[10kΩ 풀다운→GND]─┬── IRLZ44N Gate
                           │
                    Source ─┴── GND
                    Drain  ─┬── 코인 모터 (-)
                            │   코인 모터 (+) ── 3.3V
                            │       │
                            └─ 1N4001 ─┘  (밴드 쪽→3.3V)
```

**배터리 전압 분압 (GPIO34)**
```
TP4056 OUT+ ──[10kΩ R1]──┬──[10kΩ R2]── GND
                          │
                       GPIO34
Vout = Vin × R2/(R1+R2) = Vin/2
  4.2V → 2.1V,  3.0V → 1.5V  (ESP32 ADC 범위 내)
```

**NTC 온도 센서 (GPIO35)**
```
3.3V ──[10kΩ 고정]──┬── GPIO35
                     │
                [NTC 10K 프로브]
                     │
                    GND
상온 25°C: NTC=10kΩ → Vout≈1.65V
고온: NTC↓ → Vout↓  /  저온: NTC↑ → Vout↑
```

**TP4056 충전 모듈**
```
USB-C 충전 ── TP4056 ── 18650 배터리 (B+/B-)
                 │
                 ├── OUT+/OUT- → 분압회로 → GPIO34
                 └── CHRG 핀 → GPIO27 (10kΩ 풀업→3.3V)
                     충전 중: LOW  /  미충전: HIGH
```

### 주의사항

- **IRF520 모듈**: Vgs(th)≈4V로 ESP32 3.3V GPIO로는 완전 동작 불가. IRLZ44N을 사용할 것.
- **GPIO0**: ESP32 부트 모드 선택 핀. 부팅 시 LOW이면 다운로드 모드 진입. 외부 풀업 저항 필수.
- **GND 공유**: ESP32, TP4056, 배터리, 모든 센서의 GND를 반드시 공통 연결.
- **ADC 전용 핀**: GPIO34, GPIO35는 입력 전용 (출력 불가). 내부 풀업/풀다운 없음.
