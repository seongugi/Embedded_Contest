CC ?= gcc
CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
LDLIBS ?= -pthread -lm
IPC_DIR ?= ../ipc
OUTPUT_DIR := ../test
SENSOR_CPPFLAGS = $(CPPFLAGS) -I$(IPC_DIR)
SENSOR_IPC_SOURCE = $(IPC_DIR)/ipc.c

.PHONY: all aruco witness_aruco accident_aruco check check-aruco check-c check-bno check-capture check-gio bno-test-hw

GIO_CFLAGS = $(shell pkg-config --cflags gio-2.0 2>/dev/null)
GIO_LIBS = $(shell pkg-config --libs gio-2.0 2>/dev/null)
ARUCO_SOURCES = aruco_receiver.c aruco_ble.c
ARUCO_HEADERS = aruco_receiver.h aruco_ble.h $(IPC_DIR)/runtime_watchdog.h

all: aruco

aruco: witness_aruco accident_aruco

witness_aruco: $(OUTPUT_DIR)/witness_aruco

accident_aruco: $(OUTPUT_DIR)/accident_aruco

$(OUTPUT_DIR):
	mkdir -p $@

check-gio:
	@pkg-config --exists gio-2.0 || { \
		echo "Install pkg-config libglib2.0-dev (and BlueZ on the target Pi)."; exit 1; }

$(OUTPUT_DIR)/witness_aruco: witness_aruco.c $(ARUCO_SOURCES) $(ARUCO_HEADERS) \
		$(IPC_DIR)/ipc.h $(IPC_DIR)/protocol.h \
		$(IPC_DIR)/sensor_protocol.h $(SENSOR_IPC_SOURCE) | check-gio $(OUTPUT_DIR)
	$(CC) $(SENSOR_CPPFLAGS) $(GIO_CFLAGS) $(CFLAGS) witness_aruco.c \
		$(ARUCO_SOURCES) $(SENSOR_IPC_SOURCE) $(LDLIBS) $(GIO_LIBS) -o $@

$(OUTPUT_DIR)/accident_aruco: accident_aruco.c bno086.c bno086.h \
		$(ARUCO_SOURCES) $(ARUCO_HEADERS) $(IPC_DIR)/ipc.h \
		$(IPC_DIR)/protocol.h $(IPC_DIR)/accident_protocol.h \
		$(IPC_DIR)/video_processor_protocol.h $(SENSOR_IPC_SOURCE) | check-gio $(OUTPUT_DIR)
	$(CC) $(SENSOR_CPPFLAGS) $(GIO_CFLAGS) $(CFLAGS) \
		accident_aruco.c bno086.c $(ARUCO_SOURCES) \
		$(SENSOR_IPC_SOURCE) $(LDLIBS) $(GIO_LIBS) -o $@

# 실측 충돌 자료 수집기는 BLE/IPC/PAA 없이 BNO086만 사용한다.
test_collision_capture: test_collision_capture.c bno086.c bno086.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test_collision_capture.c bno086.c \
		$(LDLIBS) -o $@

check: check-aruco check-bno check-capture

# 기존 명령도 유지한다. 현재 모든 회귀시험은 C이므로 check와 같은 범위다.
check-c: check

check-aruco: | check-gio
	$(CC) $(SENSOR_CPPFLAGS) $(GIO_CFLAGS) $(CFLAGS) test/test_aruco_receiver.c \
		aruco_ble.c $(SENSOR_IPC_SOURCE) $(LDLIBS) $(GIO_LIBS) -o /tmp/test_aruco_receiver
	/tmp/test_aruco_receiver
	$(CC) $(SENSOR_CPPFLAGS) $(GIO_CFLAGS) $(CFLAGS) test/test_accident_aruco.c \
		bno086.c $(ARUCO_SOURCES) $(SENSOR_IPC_SOURCE) $(LDLIBS) $(GIO_LIBS) \
		-o /tmp/test_accident_aruco
	/tmp/test_accident_aruco
	$(CC) $(CPPFLAGS) $(GIO_CFLAGS) $(CFLAGS) test/test_aruco_ble.c \
		aruco_ble.c $(LDLIBS) $(GIO_LIBS) -o /tmp/test_aruco_ble
	/tmp/test_aruco_ble

check-bno:
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBNO086_TESTING \
		test/test_bno086.c bno086.c $(LDLIBS) -o /tmp/test_bno086
	/tmp/test_bno086 mock
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBNO086_TESTING \
		test/test_bno086_mock.c bno086.c $(LDLIBS) -o /tmp/test_bno086_mock
	/tmp/test_bno086_mock

check-capture:
	$(CC) $(CPPFLAGS) $(CFLAGS) test_collision_capture.c bno086.c \
		$(LDLIBS) -o /tmp/test_collision_capture
	/tmp/test_collision_capture --self-test

bno-test-hw:
	$(CC) $(CPPFLAGS) $(CFLAGS) test/test_bno086.c \
		bno086.c $(LDLIBS) -o /tmp/test_bno086_hw
