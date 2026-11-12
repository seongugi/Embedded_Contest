# BLE 연결만 확인하는 방법

카메라 없이 Pi 센서 수신기의 BLE 연결, GATT 서비스·특성 목록과 MTU를 확인합니다. 각 차량에 3초 연결한 뒤 해제하며 결과를 차량별 JSON에 저장합니다.

## 1. Pi 센서 서비스 준비

최초 페어링과 trust가 끝난 상태에서 진행합니다. 등록 방법은 [송신부 사용법](README.md#최초-페어링)을 따릅니다.

사고차 craft2에서:

```bash
sudo systemctl start sensor@accident.service
systemctl is-active sensor@accident.service
```

관측차 craft3·craft4·seongugi·craft6에서는 각각:

```bash
sudo systemctl start sensor@witness.service
systemctl is-active sensor@witness.service
```

`active` 상태에서 다음 연결 점검을 진행합니다. 이미 실행 중인 센서 서비스는 그대로 사용합니다.

## 2. Windows에서 차량별 연결 확인

실행 중인 `01_START_BLE.cmd`의 추적 창은 Q로 종료한 뒤, 현재 [Windows 송신 폴더](C:/Users/wooseok/Documents/Codex/2026-09-10/aruco-vision-tracking-md/outputs/ArUco_BLE_송신)에서 PowerShell을 열어 아래 명령을 차례로 실행합니다.

```powershell
.\06_CHECK_BLE_CONNECTION.cmd --name ACE1 --address 88:A2:9E:9F:DA:8E --output craft2_ble_connection.json
.\06_CHECK_BLE_CONNECTION.cmd --name ACE2 --address 88:A2:9E:48:D3:A1 --output craft3_ble_connection.json
.\06_CHECK_BLE_CONNECTION.cmd --name ACE3 --address 88:A2:9E:48:D6:44 --output craft4_ble_connection.json
.\06_CHECK_BLE_CONNECTION.cmd --name ACE4 --address 88:A2:9E:22:4D:75 --output seongugi_ble_connection.json
.\06_CHECK_BLE_CONNECTION.cmd --name ACE5 --address 88:A2:9E:48:D6:B5 --output craft6_ble_connection.json
```

각 명령이 끝나면 아무 키나 눌러 다음 명령으로 넘어갑니다. 장치를 교체했다면 [ble_config.json](ble_config.json)에 맞춰 이름·주소를 사용합니다.

정상이면 `Connected`, `service=True`, `POSE=True`, `CLOCK=True`가 나오고, 3초 후 `Disconnecting`으로 끝납니다. 검색은 최대 10초, 연결은 최대 30초 기다립니다. 좌표 전송에 필요한 MTU는 43 이상입니다. JSON의 `connected`, `held_connected_3_seconds`, `disconnected`가 모두 `true`인지 확인합니다.

이 점검은 연결과 서비스 목록을 조회합니다. 실제 좌표를 받을 때는 카메라를 연결하고 `01_START_BLE.cmd`를 실행해 `ready`와 Pi의 좌표 수신 로그를 확인합니다.

## 이전 ACPTEST1 시험

초기 연결 시험은 `pi_ble_probe.py`의 임시 `ACPTEST1` 서비스로 진행했습니다. 별도로 그 서비스를 실행한 경우 `06_CHECK_BLE_CONNECTION.cmd`를 옵션 없이 사용합니다. 차량 센서 서비스 점검에는 위 ACE1~ACE5 명령을 사용합니다.
