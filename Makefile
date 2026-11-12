#==================================================
# 모듈 디렉터리에 복사해 사용하는 Makefile 예시
# 루트 전체 빌드는 제공하지 않는다. 실제 모듈의 Makefile을 사용한다.
#==================================================

# 출력될 실행 파일 이름
# 예) sensor, filter, camera
OUTPUT =

# 컴파일할 소스 파일(.c)
# main()이 포함된 소스 파일 1개와
# 기능별 구현(.c) 파일들을 공백으로 나열
#
# 예)
# SRCS = sensor.c imu.c optical_flow.c filter.c
#
SRCS =

# IPC 라이브러리 소스
# 프로젝트 구조가 변경되지 않는 한 수정하지 않음
IPC_SRCS = ../ipc/ipc.c

# IPC 헤더 파일 위치
# 프로젝트 구조가 변경되지 않는 한 수정하지 않음
INCLUDE = -I../ipc

# 실행 파일 출력 위치
# 프로젝트 구조가 변경되지 않는 한 수정하지 않음
OUTDIR = ../test

#==================================================
# Build
#==================================================

# 빈 템플릿을 저장소 루트에서 실행해 상위 디렉터리에 파일을 만들지 않는다.
ifeq ($(strip $(OUTPUT) $(SRCS)),)
all clean:
	@echo "이 파일은 모듈용 Makefile 예시입니다."
	@echo "빌드는 각 모듈 README를 참고하세요 (예: make -C filter)."
else
all:
	mkdir -p $(OUTDIR)
	$(CC) $(INCLUDE) $(SRCS) $(IPC_SRCS) -o $(OUTDIR)/$(OUTPUT)

#==================================================
# Clean
#==================================================

clean:
	rm -f $(OUTDIR)/$(OUTPUT)
endif

.PHONY: all clean
