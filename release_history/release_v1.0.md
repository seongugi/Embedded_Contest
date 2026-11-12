# Raspberry Pi 5 Cross Compilation SDK

## 개발 환경

- Buildroot 2025.05.1
- Target: Raspberry Pi 5
- Architecture: AArch64
- C Library: glibc
- GCC 13

## 포함 항목

- Cross Compiler
- Binutils
- Sysroot
- Development Libraries
- relocate-sdk.sh

## 사용 방법

프로젝트 루트에서 아래 스크립트를 실행하면 최신 SDK를 자동으로 다운로드하고 설치합니다.

```bash
./scripts/setting/download_sdk.sh
```

SDK 환경을 활성화하려면 다음 명령을 실행합니다.

```bash
source scripts/setting/setup_sdk.sh
```

## 참고

일반 개발자는 Buildroot를 설치하거나 SDK를 직접 생성할 필요가 없습니다.

SDK 재생성이 필요한 경우 `toolchain/README.md`를 참고하시기 바랍니다.
