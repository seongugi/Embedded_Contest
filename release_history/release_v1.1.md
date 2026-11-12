# Release History

## v1.1

릴리즈 일자: 2026-08-13

### 변경 사항

- Raspberry Pi 5 AArch64 대상 sysroot를 최신 타깃 환경 기준으로 갱신했습니다.
- C 기반 업로드 큐에서 필요한 SQLite 개발 및 런타임 의존성을 반영했습니다.
- 타깃 환경 의존성 설정을 보완했습니다.
- 현재 프로젝트의 크로스 컴파일 요구사항에 맞춰 SDK를 갱신했습니다.
- GitHub Release의 파일 크기 제한을 고려하여 크로스 컴파일에 필요하지 않은 일부 타깃 전용 구성요소를 SDK 배포본에서 제외했습니다.

### SDK 구성

- AArch64 타깃 sysroot
- 타깃 개발 헤더
- 타깃 공유 라이브러리
- pkg-config 메타데이터

크로스 컴파일러와 Binutils는 호스트 시스템에서 별도로 설치하며, 본 SDK Release에는 포함하지 않습니다.

## v1.0

### Initial Release

Raspberry Pi 5 프로젝트 개발에 필요한 타깃 sysroot와 기본 SDK 환경을 제공하는 최초 릴리즈입니다.
