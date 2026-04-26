# NUCLEO-G071RB ADC 1채널 예제

## Zephyr setting 
``` 터미날
cd %HOMEPATH%
py -3.12 -m venv zephyrproject\.venv
zephyrproject\.venv\Scripts\activate.bat
cd zephyrproject

if you need,

west update
west zephyr-export
```

## Notice

NUCLEO-G071RB에서 Arduino A0를 1초마다 ADC로 읽고 UART 콘솔에 raw 값과 mV 값을 출력합니다.
G431RB용 예제와 최대한 같은 구조로 만들었고, ADC 채널 번호만 G071RB에 맞게 `0`으로 사용합니다.

## ADC/DAC 연결

- ADC 입력: Arduino A0
- MCU 핀: PA0
- ADC: ADC1
- ADC 채널: 0
- ADC resolution: 12 bit
- ADC acquisition time: `ADC_ACQ_TIME_MAX`
- ADC oversampling: STM32 LL에서 ratio 256, right shift 4
- DAC 출력: DAC1_OUT1, PA4, Arduino A2, output buffer disabled

ADC 성능 테스트용으로 Arduino A2(DAC 출력)를 Arduino A0(ADC 입력)에 점퍼로 연결하세요.
DAC 목표 전압은 3 mV입니다. 12-bit DAC와 3.3 V 기준에서는 가장 가까운 raw 값 `4`를 사용하며 실제 이상값은 약 3.22 mV입니다.
3 mV처럼 GND에 가까운 DAC 출력 테스트를 위해 DAC output buffer는 비활성화했습니다.

## 파일 구성

```text
apps/nucleo_g071rb_adc_1ch/
+-- CMakeLists.txt
+-- prj.conf
+-- boards/
|   +-- nucleo_g071rb.overlay
+-- src/
    +-- main.c
```

## 빌드

워크스페이스 루트(`c:\Users\kangc\zephyrproject`)에서 실행합니다.

```powershell
west build -b nucleo_g071rb apps\nucleo_g071rb_adc_1ch
```

클린 빌드가 필요하면:

```powershell
west build -p always -b nucleo_g071rb apps\nucleo_g071rb_adc_1ch
```

## 플래시

```powershell
west flash
```

## 예상 출력

```text
NUCLEO-G071RB ADC sample started: adc@40012400 channel 0
DAC test output: dac@40007400 channel=1 raw=4 target=3 mV pin=PA4/Arduino A2, jumper A2 to A0
DAC registers: DHR12R1=4 DOR1=4
STM32 LL ADC config: channel=0 sampling=160.5 cycles oversampling_ratio=256 shift=4
STM32 LL ADC calibration: factor=... CALFACT=0x...
[0] ll_channel=0 sampling=160.5 cycles ovs_ratio=256 shift=4 raw16=... raw12=... dac_dor1=4 voltage=... mV
```

`raw16`은 12-bit ADC를 256회 oversampling 한 뒤 4 bit right shift 한 값입니다.
`raw12`는 `raw16 >> 4` 값이고, 전압 변환은 `raw12` 기준으로 계산합니다.
