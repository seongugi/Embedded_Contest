# Windows / Raspberry Pi 5 ArUco 성능 비교

`benchmark_aruco.py`는 `track_aruco_4k.make_detector()`와 같은 설정으로 **BGR → grayscale → ArUco 검출** 구간의 처리 시간과 처리량을 측정한다. 캘리브레이션·3D 자세 계산·좌표 변환·필터·BLE·GUI까지 포함한 전체 추적 성능과 좌표 정확도는 별도 시험으로 평가한다.

Python 3.10 이상, NumPy, `cv2.aruco.ArucoDetector`가 있는 OpenCV가 필요하다. 이 폴더의 추적 코드와 함께 실행한다. OpenCV가 이미 설치돼 있으면 설정을 바꾸지 않고 사용할 수 있다. 기본값은 4K **3840×2160**, 이미지 최대 33장, 반복 3회, 준비 실행 3회다. 33장 BGR 원본은 약 821MB 메모리를 사용한다.

## 동일 이미지로 연산 비교

Windows와 Pi에 **같은 파일들**을 복사하고 각각 실행한다.

```powershell
python benchmark_aruco.py --images "C:\Users\wooseok\charuco_calibration_frames" --output windows_detection.json
```

```bash
python3 benchmark_aruco.py --images /path/to/charuco_calibration_frames --output pi5_detection.json
```

이미지 파일 읽기·JPEG 디코딩은 전부 측정 전에 끝낸다. `grayscale`, `detect`, `combined` 각각 평균·p50·p95·최댓값, 실제 프레임 크기, 검출 수와 ID, 처리량을 JSON에 기록한다. `measured_loop_fps`는 반복문 전체, `compute_only_throughput_fps`는 grayscale+detect 시간 합 기준이다. 두 장치에서 같은 입력과 명령으로 얻은 연산 측정값을 비교한다.

기본적으로 지정 해상도와 다르면 중단한다. 이미지에 한해서 의도적인 다른 크기 비교는 `--allow-other-size`로 허용할 수 있으며 실제 크기를 모두 기록한다. `--max-images 10 --repeats 3`처럼 메모리·반복 횟수를 조절할 수 있지만 두 장치에는 같은 조건을 적용해야 한다.

## 실제 카메라 포함 측정

다른 프로그램의 카메라 사용을 끝낸 뒤 실행한다.

```powershell
python benchmark_aruco.py --camera-index 1 --seconds 30 --fourcc MJPG --output windows_camera.json
```

```bash
python3 benchmark_aruco.py --camera-index 0 --seconds 30 --fourcc MJPG --output pi5_camera.json
```

Windows는 기본 DirectShow, Linux는 V4L2를 사용한다. 필요할 때 `--backend any`로 명시한다. 4K/30fps/MJPG를 요청하고 실제 반환 해상도 3840×2160을 확인한 뒤 처리량을 계측한다. 해상도가 일치하지 않으면 종료한다. 다른 해상도 시험은 `--width 1920 --height 1080`처럼 명시한다.

`read_decode`는 `VideoCapture.read()`에 걸린 시간, `read_and_compute`는 읽기부터 검출까지의 시간이다. 순차 읽기·검출 루프의 실측 FPS에는 읽기·디코딩이 포함된다. 통계는 호스트의 순차 읽기·검출 루프를 기준으로 계산한다. 센서 노출·카메라 내부 버퍼 지연과 별도 카메라 스레드를 사용하는 전체 추적 프로그램의 FPS는 별도 측정 대상이다. 준비 프레임은 `--warmup 0`에서도 최소 한 장 읽고 통계에서 제외한다.

## 비교할 때 함께 볼 값

- 두 장치의 동일 입력·실제 프레임 크기·OpenCV 버전·스레드 수·검출 ID/개수를 확인한다. ChArUco 사진과 차량 마커 5개 장면은 연산 부하가 다를 수 있다.
- 기본 OpenCV 스레드 설정을 사용하며 `--threads 4`로 지정할 수 있다. 냉각·전원·동시 실행 작업을 함께 기록한다.
- Linux에서는 실행 전후 CPU 정보와 `/sys/class/thermal` 온도를 조회하고, `vcgencmd`가 설치된 경우 `get_throttled` 상태도 기록한다. CPU 주파수·전원·팬 설정은 그대로 유지하며 현재 상태와 과거 throttle 기록을 구분해 해석한다.
- 완료된 측정의 JSON은 `--output` 경로에 저장하며, 생략하면 stdout으로 출력한다.
