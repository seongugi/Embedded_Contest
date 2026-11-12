# RPi5 SDK v1.2

릴리즈 일자: 2026-10-01

- Raspberry Pi OS 64-bit(Debian 13) 개발 헤더·공유 라이브러리·pkg-config 구성을 반영했습니다.
- GLib/GIO, libgpiod v2, libcamera, FFmpeg, SQLite, OpenSSL과 ONNX Runtime 1.29.0을 포함했습니다.
- ARM UAPI 헤더와 C++ 런타임 링크를 보완했습니다.
- SDK 환경 설정이 Pi용 헤더·시작 오브젝트·라이브러리를 우선 선택하고 ONNX 경로를 자동 적용합니다.
- GitHub Actions도 같은 SDK 자산과 SHA-256을 사용합니다.

기존 `download_sdk.sh`로 설치하고 `source scripts/setting/setup_sdk.sh`로 적용합니다. 크로스 컴파일러는 `setup_host.sh`로 GCC/G++ 14를 준비합니다.
