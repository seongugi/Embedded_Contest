# ==================================================
# Build Configuration
# ==================================================

# 생성할 실행 파일 이름
OUTPUT = witness_filter

# Filter 본체
SRCS = witness_filter.c

# 공용 IPC 구현
IPC_SRCS = ../ipc/ipc.c

# 공용 Header 경로
INCLUDE = -I../ipc

# 실행 파일 출력 위치
OUTDIR = ../test

# 컴파일 옵션
CFLAGS = -Wall -Wextra -O2

# pthread + math + Witness Candidate GPIO LED
LDFLAGS = -pthread -lm -lgpiod

# Geometry/configuration tests run on the development host without GPIO or IPC.
HOST_CC ?= cc
TEST_OUTPUT = build/test_witness_fov


# ==================================================
# Build
# ==================================================

all:
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $(INCLUDE) $(SRCS) $(IPC_SRCS) -o $(OUTDIR)/$(OUTPUT) $(LDFLAGS)

test: $(TEST_OUTPUT)
	./$(TEST_OUTPUT)

$(TEST_OUTPUT): tests/test_witness_fov.c witness_fov.h witness_radius_config.h
	mkdir -p build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 tests/test_witness_fov.c -o $@ -lm


# ==================================================
# Clean
# ==================================================

clean:
	rm -f $(OUTDIR)/$(OUTPUT)
	rm -f $(TEST_OUTPUT)

.PHONY: all test clean
