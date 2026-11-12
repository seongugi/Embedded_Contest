# 실제 Pi 수신부 호환 규약: ACP1 / ACT1

좌표 계산과 시계 처리는 [전체구조_이해하기.md](전체구조_이해하기.md), 실행 순서는 [README.md](README.md)를 참고한다.

Windows는 **ACP1 40바이트 위치 패킷**으로 측정한 좌표·방향을 전달하고, Pi의 **ACT1 20바이트 시계 응답**으로 차량별 측정 시각을 환산한다.

## GATT 연결

| 용도 | UUID | 동작 |
|---|---|---|
| Service | `7b9c0001-6a54-4f1b-9f65-2c7b8d1e4a90` | Pi Peripheral, Windows Central |
| POSE | `7b9c0002-6a54-4f1b-9f65-2c7b8d1e4a90` | PC → Pi, **40바이트 write with response** |
| CLOCK | `7b9c0003-6a54-4f1b-9f65-2c7b8d1e4a90` | PC가 read, Pi가 **20바이트 ACT1** 반환 |

각 Pi에 서로 다른 차량 ID와 광고 이름 `ACE1`~`ACE5`를 설정한다. Windows는 설정된 이름 또는 주소로 대상을 선택하고 MTU, CLOCK 응답 형식·epoch를 검사한다. Pi는 패킷 vehicle_id를 자신의 ID와 비교한다. Pi의 `--peer`와 페어링·trust 대상에는 실제 Windows Bluetooth 주소를 지정한다.

**ATT MTU는 최소 43이다.** 40바이트 payload와 3바이트 ATT 헤더를 한 번의 write request로 전송한다. Windows는 GATT 서비스를 새로 조회하고 최초 ACT1 읽기 후 MTU를 확인한다. 이후 `response=True`로 위치를 쓴다. [Bluetooth ATT 규격 §3.4.5.1](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/attribute-protocol--att-.html), [Bleak GATT 쓰기 설명](https://bleak.readthedocs.io/en/latest/api/client.html#bleak.BleakClient.write_gatt_char)

## ACT1: CLOCK read 응답 20바이트

`struct '<4sQQ'`, 정수 little-endian.

| 오프셋 | 크기 | 형식 | 내용 |
|---:|---:|---|---|
| 0 | 4 | bytes | ASCII `ACT1` |
| 4 | 8 | uint64 | 현재 수신부 epoch, **0 불가** |
| 12 | 8 | uint64 | read 처리 중 측정한 Pi `CLOCK_MONOTONIC` nanoseconds, **0 불가** |

각 차량 연결 직후 CLOCK를 5회 읽고 RTT가 작은 유효 표본을 사용한다. 이후 약 10초마다 3회 읽어 갱신한다. 재연결 시 초기 동기화를 다시 하며, epoch가 변경되면 해당 세션의 순번·시각 기준을 초기화한다. CLOCK는 매 read 시점의 값을 반환하고, 같은 epoch의 표본끼리 최소 RTT를 비교한다. **ACT1 read는 현재 세션 정보를 조회하는 동작이다.**

```python
t0 = time.monotonic_ns()                 # CLOCK read 직전
raw = await client.read_gatt_char(CLOCK_UUID, use_cached=False)
t1 = time.monotonic_ns()                 # read 완료 직후, 파싱보다 먼저
clock = decode_clock(raw)
sample = calculate_clock_sample(t0, t1, clock)
# sample.offset_ns = clock.pi_ns - t1 - 1_000_000
capture_pi_ns = capture_pc_ns + sample.offset_ns
```

RTT=`t1-t0`가 **100ms 초과면 폐기**한다. `pi_ns-t1`에 응답 지연을 반영하고 1ms의 여유를 더 빼서 관측 시각을 보수적으로 환산한다. 주기적 CLOCK 읽기로 두 장치의 시계 차이를 갱신한다.

Pi ↔ 서버는 NTP를 사용하고, PC ↔ Pi의 프레임 시각은 CLOCK 교환으로 환산한다. PC 카메라는 `time.monotonic()` 초 값을 `round(seconds * 1_000_000_000)`로 변환하며 CLOCK read 전후는 같은 기준의 `time.monotonic_ns()`로 잰다. [Python 시간 함수 설명](https://docs.python.org/3/library/time.html#time.monotonic_ns)

`capture_pc_ns`는 Windows의 `VideoCapture.read()` 완료 직후 시각이다. CLOCK 교환은 이 시각을 Pi monotonic 기준으로 환산한다. 환산한 capture가 마지막 쓰기 시도에 예약한 capture보다 클 때 새 관측을 전송한다. 카메라 버퍼 지연은 별도의 영상 지연 측정 항목으로 둔다.

## ACP1: POSE write 40바이트

`struct '<4sHHQIQiiHH'`, 모든 정수 little-endian. C 송신부도 아래 40바이트 배치에 맞춰 직렬화한다.

| 오프셋 | 크기 | 형식 | 내용 |
|---:|---:|---|---|
| 0 | 4 | bytes | ASCII `ACP1` |
| 4 | 2 | uint16 | vehicle_id, 1..65535; 수신 차량 설정과 일치 |
| 6 | 2 | uint16 | flags: 1=유효 관측, 0=무효 관측; 그 외 불가 |
| 8 | 8 | uint64 | 최신 CLOCK에서 읽은 epoch |
| 16 | 4 | uint32 | sequence |
| 20 | 8 | uint64 | 원래 관측 시각을 Pi monotonic 시계로 환산한 capture_pi_ns, **0 불가** |
| 28 | 4 | int32 | X, mm 반올림 정수 |
| 32 | 4 | int32 | Y, mm 반올림 정수 |
| 36 | 2 | uint16 | heading, 0..35999 (0.01도 단위) |
| 38 | 2 | uint16 | reserved=0 |

좌표는 finite 값을 반올림하며 확인된 수신부 한계 **-1,000,000~+1,000,000mm**를 지킨다. heading은 `round((heading_deg % 360) * 100) % 36000`이다. **0도=경기장 +Y, 90도=+X**이며, 기존 마커 +X 기준 CCW yaw를 그대로 넣으면 안 된다. Windows 추적 연결부가 3D 자세와 마커 장착 방향/위치 오프셋을 사용해 수신 규약의 차량 heading과 기준점으로 변환한 뒤 pack 함수에 넣는다. 차량 마커 높이는 고정하지 않는다.

```python
wire = pack_pose(vehicle_id, sequence, capture_pi_ns,
                 x_mm, y_mm, heading_deg, epoch=clock.epoch)
await client.write_gatt_char(POSE_UUID, wire, response=True)
```

마커를 못 읽은 **새 프레임**은 `pack_invalid(...)`로 같은 형식을 만들고 flags=0, X=Y=heading=0으로 보낸다. epoch, sequence, capture는 해당 프레임 기준으로 설정한다. Pi는 flags로 측정의 유효 여부를 판단한다.

## 현재성과 재연결 경계

- 차량별 sequence는 uint32 modulo 증가이며, 유효·무효 관측 모두 같은 번호 공간을 쓴다. 수신부는 sequence와 capture가 **모두** 전진해야 받아들인다. 같은 프레임을 더 큰 sequence만 붙여 재전송하지 않는다.
- capture 또는 Pi 수신 시각이 0이면 거절한다. 새 관측의 capture가 Pi 기준 미래이면 거절한다. 나이가 **300ms 이상**인 패킷은 위치에 적용하지 않고 정상 GATT Write Response로 수신 처리를 마친다. 연결은 유지하며 다음 최신 좌표를 받는다. 300ms 시점부터 마지막 관측도 만료한다.
- 잘못된 패킷·역순 패킷은 거절 횟수만 증가시키고 기존 관측을 덮어쓰지 않는다. 중복·역순은 정상 GATT Write Response로 처리해 연결을 유지한다. 기존 관측의 시간 만료 또는 받아들인 flags=0 관측이 위치를 무효로 만든다. 새 순번의 만료 패킷도 시각·순번 경계를 전진시키며 현재 위치를 무효로 만든다. 송신 전 크기·epoch·값 범위·시각·순번을 검사하며, 시각을 인위적으로 최신값으로 바꾸어 통과시키지 않는다.
- 연결이 끊기면 대기 관측을 버린다. 재연결은 CLOCK를 먼저 읽고 초기 시간 보정이 완료된 이후의 새 관측을 보낸다. **epoch가 같으면 sequence와 직전 capture 기준을 유지하고 순번을 계속 증가시킨다. epoch가 바뀌었을 때만 순번·capture 기준을 초기화한다.** 시계 offset은 새 CLOCK 표본으로 갱신한다. 연결 재수립이나 ACT1 읽기만으로 순번을 초기화하지 않는다. Pi 수신기는 연결 또는 보안 상태 해제 시 epoch를 갱신한다. Windows는 재연결 시 읽은 epoch로 순번·capture 기준을 맞춘다.
- Write 응답 유실은 Pi의 패킷 미수신을 의미하지 않는다. 송신기는 쓰기를 시도하기 전에 해당 순번과 capture 기준을 예약하고, 같은 epoch로 재연결한 뒤에도 그 값을 재사용하지 않는다. 따라서 응답 유실 뒤 순번이 건너뛸 수 있으며, 이 경우에도 수신부의 순번·capture 전진 규칙을 지킨다.
- 송신 대기열에는 차량별 최신 프레임 하나만 둔다. 이미 진행 중인 write 하나와 다음 대기 값 하나는 동시에 존재할 수 있으며, 새 프레임이나 무효화가 이미 시작한 write를 취소하지는 않는다. 재연결 후 대기 중이던 과거 좌표를 재생하지 않는다.

수신부는 패킷 길이·헤더·시계·순번·좌표 범위를 검사한다. 경기장 배치 후에는 알려진 위치·방향과 측정값을 비교한다.

## API와 오프라인 시험

`aruco_ble_protocol.py`는 추가 라이브러리 없이 `pack_pose`, `pack_invalid`, `pack_clock`, `decode_pose`, `decode_clock`, `calculate_clock_sample`을 제공한다. `ClockSample.map_capture()`는 원래 PC 시각을 Pi 시각으로 환산한다. `SequenceTracker`는 uint32 순환 순번을 관리하며 송수신부에서 epoch·차량·capture 전진·측정값 나이를 함께 검사한다.

```powershell
python -m unittest -v test_ble_protocol.py
```

이 명령으로 wire 바이트, 좌표·heading 범위, 무효 프레임, RTT·유효성 경계와 uint32 순환 규칙을 점검할 수 있다.

## 두 순번과 상태의 의미

카메라 프레임 번호는 read 성공마다 증가하고 처리 중 최신 프레임을 선택하면서 건너뛸 수 있다. 패킷 sequence는 차량별 쓰기 시도마다 증가하는 별도 번호다. 응답을 잃은 쓰기도 순번을 사용하므로 sequence와 `sent_pose`/`sent_lost` 성공 횟수는 같지 않을 수 있다.

`ready`는 연결과 초기 시간 보정이 완료된 상태다. `sent_pose`는 유효 위치 write의 GATT 응답 횟수, `sent_lost`는 무효 위치 write의 GATT 응답 횟수다. Pi 로그에는 패킷 채택·거절과 IPC 전달 결과를 기록한다.

실행 절차는 `README.md`에서 확인할 수 있다.

Windows는 `ble_sequence_state.json`에 차량별 epoch와 순번 예약 범위를 보관한다. 송신기를 다시 실행해도 같은 epoch에서는 이전 예약 범위 다음부터 전송하고, 새 epoch에서는 1부터 시작한다. 좌표는 저장하지 않으며 차량마다 최신 대기값 하나만 유지한다.
