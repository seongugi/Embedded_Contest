# ==================================================
# Build Configuration
# ==================================================

# 실행 파일 출력 위치
OUTDIR = ../test

# 테스트용 소스 위치
TESTDIR = for_test

# 공용 IPC 구현
IPC_SRCS = ../ipc/ipc.c

# BTP 구현
BTP_SRCS = $(wildcard btp/*.c)

# DENM 구현
DENM_SRCS = $(wildcard denm/*.c)

# asn1c 생성 파일
# converter-sample.c는 제외
ASN1_SRCS = $(filter-out \
	generated/converter-sample.c, \
	$(wildcard generated/*.c))

# Header 경로
INCLUDE = \
	-I../ipc \
	-Ibtp \
	-Idenm \
	-Igenerated

# 컴파일 옵션
CFLAGS = -std=gnu11 -Wall -Wextra -O2

# pthread
LDFLAGS = -pthread


# ==================================================
# Output Files
# ==================================================

SENDER = broadcast_sender
RECEIVER = broadcast_receiver
SENSOR_DUMMY = sensor_dummy
FILTER_DUMMY = filter_dummy
SENSOR_DUMMY_YES = sensor_dummy_yes
LAUNCHERS = $(OUTDIR)/p2p_go.sh $(OUTDIR)/p2p_client.sh


# ==================================================
# Build
# ==================================================

all: sender receiver

# 실행에 필요한 스크립트도 바이너리와 같은 test/에 배치한다.
launchers: $(LAUNCHERS)

$(OUTDIR)/%.sh: %.sh
	mkdir -p $(OUTDIR)
	install -m 0755 $< $@


# --------------------------------------------------
# Broadcast Sender
# --------------------------------------------------

sender: launchers
	mkdir -p $(OUTDIR)
	$(CC) \
		$(CFLAGS) \
		$(INCLUDE) \
		broadcast_sender.c \
		$(IPC_SRCS) \
		$(BTP_SRCS) \
		$(DENM_SRCS) \
		$(ASN1_SRCS) \
		-o $(OUTDIR)/$(SENDER) \
		$(LDFLAGS)


# --------------------------------------------------
# Broadcast Receiver
# --------------------------------------------------

receiver: launchers
	mkdir -p $(OUTDIR)
	$(CC) \
		$(CFLAGS) \
		$(INCLUDE) \
		broadcast_receiver.c \
		$(IPC_SRCS) \
		$(BTP_SRCS) \
		$(DENM_SRCS) \
		$(ASN1_SRCS) \
		-o $(OUTDIR)/$(RECEIVER) \
		$(LDFLAGS)


# --------------------------------------------------
# Sensor Dummy
# --------------------------------------------------

sensor:
	mkdir -p $(OUTDIR)
	$(CC) \
		$(CFLAGS) \
		-I../ipc \
		$(TESTDIR)/sensor_dummy.c \
		$(IPC_SRCS) \
		-o $(OUTDIR)/$(SENSOR_DUMMY)


# --------------------------------------------------
# Sensor Dummy - YES Case
# --------------------------------------------------

sensor_yes:
	mkdir -p $(OUTDIR)
	$(CC) \
		$(CFLAGS) \
		-I../ipc \
		$(TESTDIR)/sensor_dummy_yes.c \
		$(IPC_SRCS) \
		-o $(OUTDIR)/$(SENSOR_DUMMY_YES)


# --------------------------------------------------
# Filter Dummy
# --------------------------------------------------

filter:
	mkdir -p $(OUTDIR)
	$(CC) \
		$(CFLAGS) \
		-I../ipc \
		filter_dummy.c \
		$(IPC_SRCS) \
		-o $(OUTDIR)/$(FILTER_DUMMY)


# ==================================================
# Clean
# ==================================================

clean:
	rm -f $(OUTDIR)/$(SENDER)
	rm -f $(OUTDIR)/$(RECEIVER)
	rm -f $(OUTDIR)/$(SENSOR_DUMMY)
	rm -f $(OUTDIR)/$(FILTER_DUMMY)
	rm -f $(OUTDIR)/$(SENSOR_DUMMY_YES)
	rm -f $(LAUNCHERS)


# ==================================================
# Phony Targets
# ==================================================

.PHONY: all clean sender receiver launchers sensor sensor_yes filter