
/*
 * ============================================================================
 * broadcast_receiver.c
 * ============================================================================
 *
 * [이 파일의 역할]
 *
 * Wi-Fi Direct Client 쪽에서 동작하는 Receiver 프로세스다.
 *
 * 이 프로세스는 크게 3개의 Thread를 가진다.
 *
 *   1) Receive Thread
 *      - UDP recvfrom()을 전담한다.
 *      - BTP destination port로 DENM / HEARTBEAT / ACK / CAM을 구분한다.
 *      - DENM은 직접 디코딩하지 않고 DENM Queue에 복사해서 DENM Thread에 넘긴다.
 *      - HEARTBEAT는 즉시 처리한다.
 *
 *   2) DENM Thread
 *      - Receive Thread가 Queue에 넣은 DENM을 하나씩 꺼낸다.
 *      - ASN.1 UPER DENM을 decode_denm()으로 복호화한다.
 *      - DENM_t에서 Filter가 사용하는 accident_event_t만 복원한다.
 *      - DENM을 보낸 GO에게 ACK을 보낸다.
 *      - 변환한 사고 정보를 /tmp/filter.sock을 통해 Filter 프로세스에 IPC 전달한다.
 *
 *   3) Heartbeat Thread
 *      - 시작 직후에는 능동 감시를 하지 않고 기다린다.
 *      - GO가 HEARTBEAT REQUEST를 보내온 것을 Receive Thread가 확인하면
 *        monitoring_enabled가 true가 되고 그때부터 GO 능동 감시를 시작한다.
 *      - GO에게 REQUEST를 보내고 RESPONSE를 최대 1초 기다린다.
 *      - GO가 연속 3회 응답하지 않으면 장애로 판단하고 전체 종료를 요청한다.
 *
 *
 * [가장 중요한 설계 원칙 1 : recvfrom() 단일화]
 *
 * 이 프로세스에서도 UDP recvfrom()은 Receive Thread 한 곳에서만 호출한다.
 * DENM Thread와 Heartbeat Thread는 UDP를 직접 수신하지 않는다.
 *
 *
 * [가장 중요한 설계 원칙 2 : DENM Queue]
 *
 * DENM 디코딩 + 구조 변환 + Filter IPC는 상대적으로 오래 걸릴 수 있다.
 * 이것을 Receive Thread 안에서 직접 수행하면 그동안 recvfrom()이 멈춰
 * Heartbeat 같은 다른 UDP 패킷을 제때 받지 못할 수 있다.
 *
 * 그래서 Receive Thread는 DENM을 받으면:
 *
 *      "복사해서 Queue에 넣기"
 *
 * 까지만 하고 즉시 다시 recvfrom()으로 돌아간다.
 *
 * 실제 무거운 처리는 DENM Thread가 담당한다.
 *
 *
 * [DENM 수신 흐름]
 *
 *   GO Broadcast
 *       |
 *       v
 *   Receive Thread
 *       |
 *       | enqueue_denm_job()
 *       v
 *   Circular DENM Queue
 *       |
 *       | dequeue_denm_job()
 *       v
 *     DENM Thread
 *       |
 *       ├─ decode_denm()
 *       ├─ denm_to_filter_event()
 *       ├─ send_denm_ack()
 *       └─ forward_to_filter()
 *                 |
 *                 v
 *            /tmp/filter.sock
 *
 *
 * [Heartbeat 시작 시점]
 *
 * Client가 먼저 GO 능동 감시를 시작하지 않는다.
 *
 *   GO의 HEARTBEAT REQUEST 수신
 *           |
 *           v
 *   sender_role == BTP_ROLE_GO 확인
 *           |
 *           v
 *   monitoring_enabled = true
 *           |
 *           v
 *   Heartbeat Thread 시작
 *
 * 즉, GO가 Heartbeat 동작을 시작했다는 사실을 Client가 확인한 뒤
 * Client의 능동 감시도 시작하도록 맞춘 구조다.
 *
 * ============================================================================
 */
#include "ipc.h"
#include "runtime_watchdog.h"
#include "protocol.h"
#include "filter_protocol.h"
#include "btp.h"
#include "denm_decoder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "DENM.h"
#include "INTEGER.h"

/*
 * =========================
 * 네트워크 / IPC 기본 설정
 * =========================
 *
 * UDP 5000번 포트 하나에서 BTP 패킷을 받고,
 * BTP destination port로 DENM/Heartbeat/ACK/CAM을 구분한다.
 */
#define NETWORK_PORT 5000
#define FILTER_IPC_SOCKET_PATH "/tmp/filter.sock"
#define NETWORK_BUFFER_SIZE 4096
#define DENM_QUEUE_CAPACITY 8
#define DENM_RECENT_CAPACITY 64U

#define HEARTBEAT_INTERVAL_SECONDS 2
#define HEARTBEAT_TIMEOUT_SECONDS 1
#define MAX_CONSECUTIVE_FAILURES 3

enum { WD_DENM, WD_RECEIVE, WD_HEARTBEAT };
static ace_watchdog_t watchdog;
#define WD_IDLE_MS 5000U
#define WD_DENM_PROCESS_MS 15000U

#define ITS_EPOCH_UNIX_MS 1072915200000ULL
#define ITS_LEAP_SECONDS_MS 5000ULL

/*
 * Receive Thread에서 DENM Thread로 넘기는 작업 단위.
 *
 * payload는 BTP header가 제거된 순수 DENM UPER 데이터이며,
 * sender_address는 처리 완료 후 ACK를 돌려줄 GO 주소다.
 */
typedef struct
{
    uint8_t payload[NETWORK_BUFFER_SIZE];
    size_t payload_size;
    struct sockaddr_in sender_address;

} denm_job_t;

/*
 * Receiver 프로세스 전체가 공유하는 실행 context.
 *
 * Receive / DENM / Heartbeat Thread가 함께 사용한다.
 * Heartbeat 공유 상태와 DENM queue는 서로 다른 mutex로 분리되어 있다.
 */
typedef struct
{
    /* UDP/BTP socket과 Filter IPC client socket */
    int udp_fd;
    int filter_fd;

    /* Heartbeat 대상인 GO의 주소와 연속 실패 횟수 */
    struct sockaddr_in go_address;
    char go_ip[INET_ADDRSTRLEN];
    int go_failure_count;

    /* 1이면 실행, 0이면 모든 Thread가 종료 방향으로 진행한다. */
    volatile sig_atomic_t running;

    /* DENM 처리 / Heartbeat 감시 / UDP 수신 전담 Thread */
    pthread_t denm_thread;
    pthread_t heartbeat_thread;
    pthread_t receive_thread;

    /*
     * Heartbeat RESPONSE 상태와 monitoring_enabled를 보호한다.
     * heartbeat_condition은 RESPONSE 대기,
     * monitoring_condition은 능동 감시 시작 시점 대기에 사용한다.
     */
    pthread_mutex_t heartbeat_mutex;
    pthread_cond_t heartbeat_condition;
    pthread_cond_t monitoring_condition;

    uint32_t heartbeat_sequence;
    bool heartbeat_response_received;
    uint32_t heartbeat_response_sequence;
    bool monitoring_enabled;

    /*
     * Receive Thread -> DENM Thread 사이의 circular queue를 보호한다.
     */
    pthread_mutex_t denm_queue_mutex;
    pthread_cond_t denm_queue_condition;

    denm_job_t denm_queue[DENM_QUEUE_CAPACITY];
    int denm_queue_head;
    int denm_queue_tail;
    int denm_queue_count;

} receiver_context_t;

/*
 * signal_handler에서 현재 Receiver context에 접근하기 위한 전역 포인터.
 */
static receiver_context_t *global_context = NULL;

/* ============================================================
 * 함수 원형 선언
 *
 * 실제 구현은 아래에서 main()의 실행 흐름에 맞춰 배치한다.
 * ============================================================ */

static void add_seconds(
    struct timespec *time_value,
    time_t seconds
);

static int initialize_context(
    receiver_context_t *context,
    const char *go_ip
);

static int create_udp_socket(void);

static void signal_handler(int signal_number);

static void request_stop(receiver_context_t *context);

static void *denm_thread_main(void *argument);

static bool dequeue_denm_job(
    receiver_context_t *context,
    denm_job_t *job
);

static int denm_to_filter_event(
    const DENM_t *denm,
    accident_event_t *event
);

static int send_denm_ack(
    receiver_context_t *context,
    const struct sockaddr_in *destination,
    uint32_t sequence
);

static int forward_to_filter(
    receiver_context_t *context,
    const accident_event_t *event
);

static int connect_to_filter(receiver_context_t *context);

static void *receive_thread_main(void *argument);

static bool enqueue_denm_job(
    receiver_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct sockaddr_in *sender_address
);

static void handle_heartbeat_payload(
    receiver_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct sockaddr_in *sender_address,
    const char *sender_ip
);

static void enable_monitoring(receiver_context_t *context);

static void record_heartbeat_response(
    receiver_context_t *context,
    uint32_t sequence
);

static void *heartbeat_thread_main(void *argument);

static bool check_go(receiver_context_t *context);

static int send_heartbeat_packet(
    receiver_context_t *context,
    const struct sockaddr_in *destination,
    uint32_t packet_type,
    uint32_t sequence
);

static int send_btp_payload(
    receiver_context_t *context,
    const struct sockaddr_in *destination,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_size
);

static void destroy_context(receiver_context_t *context);

/* ============================================================
 * 공통 보조 함수
 * ============================================================ */

/*
 * timespec 절대 시각에 초 단위 시간을 더한다.
 *
 * pthread_cond_timedwait()는 "몇 초 동안 기다려라"가 아니라
 * "이 절대 시각까지 기다려라" 형태의 deadline을 요구하므로,
 * clock_gettime()으로 얻은 현재 시각에 timeout 값을 더할 때 사용한다.
 */
static void add_seconds(
    struct timespec *time_value,
    time_t seconds
)
{
    time_value->tv_sec += seconds;
}

static int64_t timestamp_its_to_unix_ns(int64_t timestamp_its)
{
    int64_t unix_ms =
        timestamp_its
        + (int64_t)ITS_EPOCH_UNIX_MS
        - (int64_t)ITS_LEAP_SECONDS_MS;

    return unix_ms * 1000000LL;
}

/* ============================================================
 * 1. Receiver 초기화
 * ============================================================ */

/*
 * Receiver 전체 context를 초기화한다.
 *
 * main()에서 가장 먼저 호출되며 다음을 준비한다.
 *   - GO IP 문자열과 sockaddr_in
 *   - Heartbeat mutex/condition
 *   - DENM queue mutex/condition
 *   - CLOCK_MONOTONIC 기반 condition variable
 *   - UDP 5000번 socket
 *
 * 중간 단계에서 실패하면 이미 생성된 자원을 역순으로 정리한다.
 */
static int initialize_context(
    receiver_context_t *context,
    const char *go_ip
)
{
    memset(context, 0, sizeof(*context));

    context->udp_fd = -1;
    context->filter_fd = -1;
    context->running = 1;

    if (strlen(go_ip) >= sizeof(context->go_ip))
    {
        return -1;
    }

    strcpy(context->go_ip, go_ip);

    context->go_address.sin_family = AF_INET;
    context->go_address.sin_port = htons(NETWORK_PORT);

    if (
        inet_pton(
            AF_INET,
            go_ip,
            &context->go_address.sin_addr
        ) != 1
    )
    {
        return -1;
    }

    if (
        pthread_mutex_init(
            &context->heartbeat_mutex,
            NULL
        ) != 0
    )
    {
        return -1;
    }

    if (
        pthread_mutex_init(
            &context->denm_queue_mutex,
            NULL
        ) != 0
    )
    {
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    pthread_condattr_t condition_attribute;

    if (
        pthread_condattr_init(
            &condition_attribute
        ) != 0
    )
    {
        pthread_mutex_destroy(&context->denm_queue_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    if (
        pthread_condattr_setclock(
            &condition_attribute,
            CLOCK_MONOTONIC
        ) != 0
    )
    {
        pthread_condattr_destroy(&condition_attribute);
        pthread_mutex_destroy(&context->denm_queue_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    if (
        pthread_cond_init(
            &context->heartbeat_condition,
            &condition_attribute
        ) != 0
    )
    {
        pthread_condattr_destroy(&condition_attribute);
        pthread_mutex_destroy(&context->denm_queue_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    if (
        pthread_cond_init(
            &context->monitoring_condition,
            &condition_attribute
        ) != 0
    )
    {
        pthread_cond_destroy(&context->heartbeat_condition);
        pthread_condattr_destroy(&condition_attribute);
        pthread_mutex_destroy(&context->denm_queue_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    if (
        pthread_cond_init(
            &context->denm_queue_condition,
            &condition_attribute
        ) != 0
    )
    {
        pthread_cond_destroy(&context->monitoring_condition);
        pthread_cond_destroy(&context->heartbeat_condition);
        pthread_condattr_destroy(&condition_attribute);
        pthread_mutex_destroy(&context->denm_queue_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    pthread_condattr_destroy(&condition_attribute);

    context->udp_fd = create_udp_socket();

    if (context->udp_fd < 0)
    {
        pthread_cond_destroy(&context->denm_queue_condition);
        pthread_cond_destroy(&context->monitoring_condition);
        pthread_cond_destroy(&context->heartbeat_condition);
        pthread_mutex_destroy(&context->denm_queue_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    return 0;
}

/*
 * Receiver의 UDP/BTP 수신 socket을 생성하고 5000번 포트에 bind한다.
 *
 * Sender와 달리 broadcast 송신 기능이 필요하지 않으므로 SO_BROADCAST는 설정하지 않는다.
 * 이 socket은 Receive Thread의 recvfrom()과 ACK/Heartbeat 송신에 함께 사용된다.
 */
static int create_udp_socket(void)
{
    int sockfd;
    int enable = 1;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }
    struct timeval io_timeout = {1, 0};
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
                   &io_timeout, sizeof(io_timeout)) != 0
        || setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO,
                      &io_timeout, sizeof(io_timeout)) != 0)
    {
        perror("setsockopt UDP timeout");
        close(sockfd);
        return -1;
    }

    if (
        setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enable,
            sizeof(enable)
        ) < 0
    )
    {
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in local_address;

    memset(&local_address, 0, sizeof(local_address));

    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(NETWORK_PORT);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (
        bind(
            sockfd,
            (struct sockaddr *)&local_address,
            sizeof(local_address)
        ) < 0
    )
    {
        perror("bind");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* ============================================================
 * 2. 종료 제어
 * ============================================================ */

/*
 * SIGINT / SIGTERM 수신 시 최소한의 종료 신호만 전달한다.
 *
 * running을 0으로 바꾸고 UDP/Filter IPC socket을 shutdown()해
 * blocking I/O가 빠져나오도록 한다.
 * 복잡한 pthread condition 처리는 일반 함수 request_stop()에서 담당한다.
 */
static void signal_handler(int signal_number)
{
    (void)signal_number;

    if (global_context == NULL)
    {
        return;
    }

    /* signal handler에서는 비동기 안전성을 위해 상태 변경과 socket 깨우기만 한다. */
    global_context->running = 0;

    if (global_context->udp_fd >= 0)
    {
        shutdown(global_context->udp_fd, SHUT_RDWR);
    }

    if (global_context->filter_fd >= 0)
    {
        shutdown(global_context->filter_fd, SHUT_RDWR);
    }
}

/*
 * Receiver 전체 종료를 요청한다.
 *
 * Heartbeat 대기, monitoring 시작 대기, DENM queue 대기를 모두 깨운다.
 * 이후 UDP와 Filter IPC socket을 shutdown()하여 blocking I/O도 종료한다.
 */
static void request_stop(receiver_context_t *context)
{
    if (!context->running)
    {
        return;
    }

    context->running = 0;

    pthread_mutex_lock(&context->heartbeat_mutex);
    pthread_cond_broadcast(&context->heartbeat_condition);
    pthread_cond_broadcast(&context->monitoring_condition);
    pthread_mutex_unlock(&context->heartbeat_mutex);

    pthread_mutex_lock(&context->denm_queue_mutex);
    pthread_cond_broadcast(&context->denm_queue_condition);
    pthread_mutex_unlock(&context->denm_queue_mutex);

    if (context->udp_fd >= 0)
    {
        shutdown(context->udp_fd, SHUT_RDWR);
    }

    if (context->filter_fd >= 0)
    {
        shutdown(context->filter_fd, SHUT_RDWR);
    }
}

/* ============================================================
 * 3. DENM Thread - Decode / ACK / Filter IPC
 * ============================================================ */

/* 최근 Filter 전달 성공 이력은 DENM Thread만 접근한다. */
static bool denm_was_forwarded(
    const accident_event_t *recent,
    size_t count,
    const accident_event_t *event
)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (recent[index].station_id == event->station_id
            && recent[index].sequence_number == event->sequence_number
            && recent[index].detection_time == event->detection_time)
        {
            return true;
        }
    }
    return false;
}

/*
 * [DENM Thread]
 * Receive Thread가 queue에 넣어 둔 DENM을 실제로 처리한다.
 *
 * 처리 흐름:
 *   queue dequeue
 *      -> UPER decode
 *      -> DENM 필드에서 Filter용 accident_event_t 복원
 *      -> GO에게 DENM ACK 전송
 *      -> 이미 전달한 사고이면 중복 IPC 생략
 *      -> Filter IPC(/tmp/filter.sock)로 사고 확인 데이터 전달
 *
 * 무거운 ASN.1 decode와 IPC 재연결 로직을 Receive Thread에서 분리해
 * recvfrom() 수신 경로가 오래 막히지 않게 한다.
 */
static void *denm_thread_main(void *argument)
{
    receiver_context_t *context =
        (receiver_context_t *)argument;
    /* 이 스레드에서만 Filter 전달 성공 이력을 읽고 갱신한다. */
    accident_event_t recent[DENM_RECENT_CAPACITY];
    size_t recent_count = 0;
    size_t recent_next = 0;

    while (context->running)
    {
        ace_watchdog_progress(&watchdog, WD_DENM, WD_IDLE_MS);
        denm_job_t job;

        if (!dequeue_denm_job(context, &job))
        {
            break;
        }
        ace_watchdog_progress(&watchdog, WD_DENM, WD_DENM_PROCESS_MS);

        /*
         * Queue 안 payload는 BTP header가 이미 제거된 순수 DENM UPER 데이터다.
         * 이를 asn1c가 생성한 DENM_t 구조체로 복호화한다.
         */
        DENM_t *decoded_denm = NULL;

        if (
            decode_denm(
                job.payload,
                job.payload_size,
                &decoded_denm
            ) != 0
        )
        {
            continue;
        }

        accident_event_t event;

        if (
            denm_to_filter_event(
                decoded_denm,
                &event
            ) != 0
        )
        {
            fprintf(
                stderr,
                "DENM -> IPC 사고 데이터 변환 실패\n"
            );

            ASN_STRUCT_FREE(
                asn_DEF_DENM,
                decoded_denm
            );

            continue;
        }

        /*
         * ACK은 DENM을 정상적으로 decode하고 프로젝트 내부 데이터로
         * 변환하는 데 성공한 뒤 보낸다.
         *
         * destination은 이 DENM job과 함께 저장해 둔 실제 송신자(GO) 주소다.
         */
        if (
            send_denm_ack(
                context,
                &job.sender_address,
                event.sequence_number
            ) < 0
        )
        {
            perror("DENM ACK sendto");
        }
        else
        {
            printf(
                "[ACK] DENM ACK 전송 완료 (sequence=%u)\n",
                (unsigned int)event.sequence_number
            );
        }

        /* 재수신에도 ACK를 보내고, 이미 전달한 사고의 Filter 호출만 생략한다. */
        if (denm_was_forwarded(recent, recent_count, &event))
        {
            ASN_STRUCT_FREE(asn_DEF_DENM, decoded_denm);
            continue;
        }

        /*
         * DENM 네트워크 메시지를 프로젝트 내부 사고 이벤트로 변환했으므로
         * 이제 Filter 프로세스에 IPC_MSG_ACCIDENT_CONFIRM으로 전달한다.
         *
         * Filter가 아직 올라오지 않았거나 연결이 끊기면
         * forward_to_filter() 내부에서 재연결을 시도한다.
         */
        if (
            forward_to_filter(
                context,
                &event
            ) != 0
        )
        {
            ASN_STRUCT_FREE(
                asn_DEF_DENM,
                decoded_denm
            );

            break;
        }

        recent[recent_next] = event;
        recent_next = (recent_next + 1U) % DENM_RECENT_CAPACITY;
        if (recent_count < DENM_RECENT_CAPACITY)
            ++recent_count;

        ASN_STRUCT_FREE(
            asn_DEF_DENM,
            decoded_denm
        );
    }

    return NULL;
}

/*
 * DENM circular queue에서 작업 하나를 꺼낸다.
 *
 * queue가 비어 있으면 denm_queue_condition에서 기다린다.
 * 1초 timed wait를 사용하는 것은 종료 상태를 주기적으로 다시 확인하기 위함이다.
 *
 * head를 증가시키며 배열 끝에 도달하면 modulo 연산으로 0으로 돌아온다.
 */
static bool dequeue_denm_job(
    receiver_context_t *context,
    denm_job_t *job
)
{
    pthread_mutex_lock(&context->denm_queue_mutex);

    /*
     * Queue가 비어 있으면 DENM Thread가 할 일이 없으므로 잠든다.
     * Receive Thread가 새 DENM을 enqueue하고 condition signal을 보내면 깨어난다.
     */
    while (
        context->running &&
        context->denm_queue_count == 0
    )
    {
        struct timespec wake_time;

        if (clock_gettime(CLOCK_MONOTONIC, &wake_time) == 0)
        {
            add_seconds(&wake_time, 1);

            pthread_cond_timedwait(
                &context->denm_queue_condition,
                &context->denm_queue_mutex,
                &wake_time
            );
            ace_watchdog_progress(&watchdog, WD_DENM, WD_IDLE_MS);
        }
    }

    if (
        !context->running &&
        context->denm_queue_count == 0
    )
    {
        pthread_mutex_unlock(&context->denm_queue_mutex);
        return false;
    }

    *job = context->denm_queue[context->denm_queue_head];

    context->denm_queue_head =
        (context->denm_queue_head + 1) % DENM_QUEUE_CAPACITY;
    context->denm_queue_count--;

    pthread_mutex_unlock(&context->denm_queue_mutex);

    return true;
}

/*
 * ASN.1 DENM에서 Filter와 ACK에 필요한 Action ID, 사고 시각, 로컬 좌표만 복원한다.
 * actionId.sequenceNumber는 ACK sequence와 Filter IPC sequence로 재사용한다.
 */
static int denm_to_filter_event(
    const DENM_t *denm,
    accident_event_t *event
)
{
    const ManagementContainer_t *management =
        &denm->denm.management;

    long detection_time;

    memset(event, 0, sizeof(*event));

    if (
        asn_INTEGER2long(
            &management->detectionTime,
            &detection_time
        ) != 0
    )
    {
        return -1;
    }

    event->station_id = (uint32_t)management->actionId.originatingStationId;
    event->sequence_number = (uint16_t)management->actionId.sequenceNumber;
    event->detection_time = timestamp_its_to_unix_ns((int64_t)detection_time);

    if (
        denm->denm.alacarte != NULL &&
        denm->denm.alacarte->localCoordinates != NULL
    )
    {
        event->x_mm = (float)
            denm->denm.alacarte->localCoordinates->xMillimeter;
        event->y_mm = (float)
            denm->denm.alacarte->localCoordinates->yMillimeter;
    }

    return 0;
}

/*
 * 정상적으로 decode/변환한 DENM에 대해 GO로 ACK를 보낸다.
 *
 * ACK payload에는 DENM sequence만 담고 BTP_PORT_ACK으로 전송한다.
 */
static int send_denm_ack(
    receiver_context_t *context,
    const struct sockaddr_in *destination,
    uint32_t sequence
)
{
    btp_ack_payload_t ack;

    ack.sequence = htonl(sequence);

    return send_btp_payload(
        context,
        destination,
        BTP_PORT_ACK,
        (const uint8_t *)&ack,
        sizeof(ack)
    );
}

/*
 * Filter용으로 변환된 사고 이벤트를
 * Filter 프로세스에 IPC_MSG_ACCIDENT_CONFIRM으로 전달한다.
 *
 * Filter 연결이 없으면 connect_to_filter()로 연결하고,
 * ipc_send()가 실패하면 기존 fd를 닫은 뒤 다시 연결하여 재전송을 반복한다.
 * running이 0이 되면 반복을 종료한다.
 */
static int forward_to_filter(
    receiver_context_t *context,
    const accident_event_t *event
)
{
    ipc_header_t header;

    memset(&header, 0, sizeof(header));

    header.type = IPC_MSG_ACCIDENT_CONFIRM;
    header.flags = IPC_FLAG_NONE;
    header.sequence = event->sequence_number;
    header.timestamp = 0;

    while (context->running)
    {
        ace_watchdog_progress(&watchdog, WD_DENM, WD_IDLE_MS);
        if (connect_to_filter(context) < 0)
        {
            return -1;
        }

        /*
         * 현재 연결된 Filter IPC fd로 사고 확인 메시지를 전달한다.
         * 실패하면 fd가 더 이상 유효하지 않다고 보고 닫은 뒤,
         * while 처음으로 돌아가 connect_to_filter()부터 다시 수행한다.
         */
        int result = ipc_send(
            context->filter_fd,
            &header,
            event,
            sizeof(*event)
        );

        if (result == IPC_SUCCESS)
        {
            return 0;
        }

        fprintf(
            stderr,
            "Filter IPC 전달 실패: %d\n",
            result
        );

        ipc_close(context->filter_fd);
        context->filter_fd = -1;
    }

    return -1;
}

/*
 * /tmp/filter.sock에 Client로 연결한다.
 *
 * 이미 연결된 filter_fd가 있으면 즉시 성공한다.
 * 연결되지 않은 상태라면 1초 간격으로 재시도하며 Filter가 준비되기를 기다린다.
 */
static int connect_to_filter(receiver_context_t *context)
{
    if (context->filter_fd >= 0)
    {
        return 0;
    }

    printf("Filter IPC 연결 대기 중...\n");

    /*
     * Filter가 Receiver보다 늦게 실행될 수 있으므로 한 번 실패했다고
     * Receiver를 종료하지 않고 1초 간격으로 연결을 재시도한다.
     */
    while (context->running)
    {
        /* A full Unix-socket backlog is a retry, not a stuck DENM worker. */
        ace_watchdog_progress(&watchdog, WD_DENM, WD_IDLE_MS);
        struct sockaddr_un address = {0};
        struct timeval send_timeout = {1, 0};
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd >= 0)
        {
            address.sun_family = AF_UNIX;
            memcpy(address.sun_path, FILTER_IPC_SOCKET_PATH,
                   sizeof(FILTER_IPC_SOCKET_PATH));
            if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0
                || fcntl(fd, F_SETFL, 0) != 0
                || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                              &send_timeout, sizeof(send_timeout)) != 0)
            {
                close(fd);
                fd = -1;
            }
        }

        if (fd >= 0)
        {
            context->filter_fd = fd;
            printf("Filter IPC 연결 완료\n");
            return 0;
        }

        sleep(1);
    }

    return -1;
}

/* ============================================================
 * 4. Receive Thread - UDP 수신 및 분배
 * ============================================================ */

/*
 * [Receive Thread]
 * Receiver의 UDP recvfrom()을 전담한다.
 *
 * 받은 패킷의 BTP destination port에 따라:
 *   DENM      -> payload를 DENM queue에 복사
 *   HEARTBEAT -> 즉시 Heartbeat 처리
 *   ACK       -> Receiver에서는 사용하지 않음
 *   CAM       -> 향후 확장 자리
 *
 * DENM decode 자체는 하지 않는다. queue에 넣고 곧바로 다음 UDP 패킷을 받는 것이 핵심이다.
 */
static void *receive_thread_main(void *argument)
{
    receiver_context_t *context =
        (receiver_context_t *)argument;

    uint8_t receive_buffer[NETWORK_BUFFER_SIZE];
    const char *p2p_interface = getenv("ACE_P2P_INTERFACE");
    unsigned int p2p_ifindex = 0;

    if (p2p_interface != NULL && p2p_interface[0] != '\0')
        p2p_ifindex = if_nametoindex(p2p_interface);

    while (context->running)
    {
        /* 첫 GO heartbeat 전에도 그룹 소실을 감지해 연결부터 다시 시작한다. */
        if (p2p_interface != NULL && p2p_interface[0] != '\0' &&
            (p2p_ifindex == 0 ||
             if_nametoindex(p2p_interface) != p2p_ifindex))
        {
            fprintf(stderr, "[P2P] %s 인터페이스 소실 또는 교체; 재연결\n",
                    p2p_interface);
            request_stop(context);
            break;
        }
        ace_watchdog_progress(&watchdog, WD_RECEIVE, WD_IDLE_MS);
        struct sockaddr_in sender_address;
        socklen_t sender_address_length =
            sizeof(sender_address);

            /*
         * ★ Receiver에서도 recvfrom()은 이 Thread 하나만 수행한다.
         *
         * DENM Thread는 Queue만 소비하고,
         * Heartbeat Thread는 condition variable로 RESPONSE 결과만 받는다.
         */
        ssize_t received_bytes = recvfrom(
            context->udp_fd,
            receive_buffer,
            sizeof(receive_buffer),
            0,
            (struct sockaddr *)&sender_address,
            &sender_address_length
        );

        if (received_bytes < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }

            if (!context->running)
            {
                break;
            }

            perror("recvfrom");
            continue;
        }

        uint16_t destination_port;
        size_t payload_size;

        const uint8_t *payload = btp_parse_packet(
            receive_buffer,
            (size_t)received_bytes,
            &destination_port,
            &payload_size
        );

        if (payload == NULL)
        {
            continue;
        }

        char sender_ip[INET_ADDRSTRLEN];

        if (
            inet_ntop(
                AF_INET,
                &sender_address.sin_addr,
                sender_ip,
                sizeof(sender_ip)
            ) == NULL
        )
        {
            continue;
        }

        switch (destination_port)
        {
        case BTP_PORT_DENM:
            /*
             * DENM을 여기서 바로 decode하지 않는다.
             *
             * decode + Filter IPC가 오래 걸리는 동안 recvfrom()이 멈추지 않게
             * Payload를 Queue에 복사하고 Receive Thread는 즉시 수신으로 복귀한다.
             */
            if (
                !enqueue_denm_job(
                    context,
                    payload,
                    payload_size,
                    &sender_address
                )
            )
            {
                fprintf(
                    stderr,
                    "DENM 처리 큐가 가득 차 패킷을 폐기합니다.\n"
                );
            }
            break;

        case BTP_PORT_HEARTBEAT:
            handle_heartbeat_payload(
                context,
                payload,
                payload_size,
                &sender_address,
                sender_ip
            );
            break;

        case BTP_PORT_ACK:
            /* Client Receiver에서는 DENM ACK를 수신하지 않는다. */
            break;

        case BTP_PORT_CAM:
            /* 향후 CAM 확장을 위한 분기다. */
            break;

        default:
            break;
        }
    }

    return NULL;
}

/*
 * Receive Thread가 받은 DENM payload와 송신자 주소를 circular queue에 저장한다.
 *
 * sender_address를 함께 보관하는 이유는 DENM Thread가 처리를 끝낸 뒤
 * 해당 DENM을 보낸 GO에게 ACK를 정확히 돌려주기 위해서다.
 *
 * queue가 DENM_QUEUE_CAPACITY만큼 가득 차면 false를 반환하고 패킷을 폐기한다.
 */
static bool enqueue_denm_job(
    receiver_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct sockaddr_in *sender_address
)
{
    bool queued = false;

    if (payload_size > NETWORK_BUFFER_SIZE)
    {
        return false;
    }

    pthread_mutex_lock(&context->denm_queue_mutex);

    /*
     * Queue가 가득 차지 않은 경우에만 저장한다.
     *
     * 이 Queue는 Receive Thread(생산자) -> DENM Thread(소비자)의
     * 전형적인 Producer/Consumer 구조다.
     */
    if (context->denm_queue_count < DENM_QUEUE_CAPACITY)
    {
        denm_job_t *job =
            &context->denm_queue[context->denm_queue_tail];

        memcpy(job->payload, payload, payload_size);
        job->payload_size = payload_size;
        job->sender_address = *sender_address;

        /*
         * 원형 큐(circular queue)이므로 마지막 칸 다음에는 다시 0으로 돌아간다.
         *
         * 예: capacity=8일 때 tail=7이면
         *     (7 + 1) % 8 == 0
         */
        context->denm_queue_tail =
            (context->denm_queue_tail + 1) % DENM_QUEUE_CAPACITY;
        context->denm_queue_count++;

        pthread_cond_signal(&context->denm_queue_condition);
        queued = true;
    }

    pthread_mutex_unlock(&context->denm_queue_mutex);

    return queued;
}

/*
 * GO에서 온 Heartbeat REQUEST/RESPONSE를 처리한다.
 *
 * 먼저 magic과 sender IP를 검사해 현재 지정된 GO의 패킷만 인정한다.
 *
 * REQUEST:
 *   GO 역할의 REQUEST를 처음 받으면 enable_monitoring()으로 능동 감시를 시작하고,
 *   같은 sequence의 RESPONSE를 GO에게 보낸다.
 *
 * RESPONSE:
 *   check_go()가 기다리는 응답 상태를 기록한다.
 */
static void handle_heartbeat_payload(
    receiver_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct sockaddr_in *sender_address,
    const char *sender_ip
)
{
    btp_heartbeat_payload_t heartbeat;

    if (payload_size != sizeof(heartbeat))
    {
        return;
    }

    memcpy(&heartbeat, payload, sizeof(heartbeat));

    uint32_t magic = ntohl(heartbeat.magic);
    uint32_t packet_type = ntohl(heartbeat.type);
    uint32_t sender_role = ntohl(heartbeat.role);
    uint32_t sequence = ntohl(heartbeat.sequence);

    if (magic != BTP_HEARTBEAT_MAGIC)
    {
        return;
    }

    if (strcmp(sender_ip, context->go_ip) != 0)
    {
        return;
    }

    if (packet_type == BTP_HEARTBEAT_REQUEST)
    {
        if (sender_role == BTP_ROLE_GO)
        {
            enable_monitoring(context);
        }

        if (
            send_heartbeat_packet(
                context,
                sender_address,
                BTP_HEARTBEAT_RESPONSE,
                sequence
            ) < 0
        )
        {
            fprintf(
                stderr,
                "[Heartbeat] GO 응답 전송 실패\n"
            );
        }
    }
    else if (packet_type == BTP_HEARTBEAT_RESPONSE)
    {
        record_heartbeat_response(
            context,
            sequence
        );
    }
}

/*
 * Receiver의 능동 Heartbeat 감시 시작 플래그를 켠다.
 *
 * Client가 실행되자마자 GO를 감시하지 않고,
 * GO로부터 실제 Heartbeat REQUEST를 한 번 받은 뒤에만 monitoring_enabled를 true로 만든다.
 * heartbeat_thread_main()은 monitoring_condition에서 이 순간을 기다린다.
 */
static void enable_monitoring(receiver_context_t *context)
{
    pthread_mutex_lock(&context->heartbeat_mutex);

    if (!context->monitoring_enabled)
    {
        /*
         * Client Heartbeat Thread는 프로그램 시작 즉시 능동 감시하지 않는다.
         * GO의 Heartbeat REQUEST를 실제로 한 번 수신한 시점에만
         * monitoring_enabled를 true로 바꿔 감시를 시작한다.
         */
        context->monitoring_enabled = true;

        pthread_cond_broadcast(
            &context->monitoring_condition
        );
    }

    pthread_mutex_unlock(&context->heartbeat_mutex);
}

/*
 * Receive Thread가 받은 GO의 Heartbeat RESPONSE sequence를 공유 상태에 기록하고
 * heartbeat_condition을 깨워 check_go()가 즉시 확인하도록 한다.
 */
static void record_heartbeat_response(
    receiver_context_t *context,
    uint32_t sequence
)
{
    pthread_mutex_lock(&context->heartbeat_mutex);

    context->heartbeat_response_received = true;
    context->heartbeat_response_sequence = sequence;

    pthread_cond_broadcast(
        &context->heartbeat_condition
    );

    pthread_mutex_unlock(&context->heartbeat_mutex);
}

/* ============================================================
 * 5. Heartbeat Thread - GO 생존 감시
 * ============================================================ */

/*
 * [Heartbeat Thread]
 * 처음에는 monitoring_enabled가 켜질 때까지 대기한다.
 *
 * 즉, GO가 먼저 Heartbeat REQUEST를 보내 'GO도 준비됨'을 보여 준 뒤에
 * Receiver가 능동 감시를 시작하는 구조다.
 *
 * 이후 check_go()를 반복하며 연속 실패가 임계치에 도달하면
 * GO 장애로 판단하고 Receiver 전체를 종료한다.
 */
static void *heartbeat_thread_main(void *argument)
{
    receiver_context_t *context =
        (receiver_context_t *)argument;

    ace_watchdog_progress(&watchdog, WD_HEARTBEAT, WD_IDLE_MS);
    pthread_mutex_lock(&context->heartbeat_mutex);

    /*
     * 시작 직후의 Heartbeat Thread는 여기서 대기한다.
     *
     * Receive Thread가 GO의 HEARTBEAT REQUEST를 받은 뒤 enable_monitoring()을
     * 호출해야만 monitoring_enabled가 true가 되고 다음 단계로 진행한다.
     */
    while (
        context->running &&
        !context->monitoring_enabled
    )
    {
        struct timespec wake_time;

        if (clock_gettime(CLOCK_MONOTONIC, &wake_time) == 0)
        {
            add_seconds(&wake_time, 1);

            pthread_cond_timedwait(
                &context->monitoring_condition,
                &context->heartbeat_mutex,
                &wake_time
            );
            ace_watchdog_progress(&watchdog, WD_HEARTBEAT, WD_IDLE_MS);
        }
    }

    pthread_mutex_unlock(&context->heartbeat_mutex);

    if (!context->running)
    {
        return NULL;
    }

    while (context->running)
    {
        ace_watchdog_progress(&watchdog, WD_HEARTBEAT, WD_IDLE_MS);
        if (check_go(context))
        {
            context->go_failure_count = 0;

        }
        else
        {
            if (!context->running)
            {
                break;
            }

            context->go_failure_count++;

            printf(
                "[Heartbeat] %s 응답 없음 (%d/%d)\n",
                context->go_ip,
                context->go_failure_count,
                MAX_CONSECUTIVE_FAILURES
            );

            if (
                context->go_failure_count >=
                MAX_CONSECUTIVE_FAILURES
            )
            {
                printf(
                    "[Heartbeat] %s 장애 판단\n",
                    context->go_ip
                );

                request_stop(context);
                return NULL;
            }
        }

        for (
            int i = 0;
            i < HEARTBEAT_INTERVAL_SECONDS &&
            context->running;
            i++
        )
        {
            sleep(1);
            ace_watchdog_progress(&watchdog, WD_HEARTBEAT, WD_IDLE_MS);
        }
    }

    return NULL;
}

/*
 * GO 한 대에 Heartbeat REQUEST를 보내고 같은 sequence의 RESPONSE를 기다린다.
 *
 * Receive Thread가 RESPONSE를 수신하면 record_heartbeat_response()가 상태를 기록한다.
 * HEARTBEAT_TIMEOUT_SECONDS 내에 응답이 없으면 false를 반환한다.
 */
static bool check_go(receiver_context_t *context)
{
    uint32_t current_sequence;

    pthread_mutex_lock(&context->heartbeat_mutex);

    /*
     * 새로운 GO 검사마다 sequence를 증가시킨다.
     * 늦게 도착한 과거 RESPONSE를 현재 응답으로 오인하지 않기 위한 식별자다.
     */
    context->heartbeat_sequence++;
    current_sequence = context->heartbeat_sequence;

    /* 직전 Heartbeat RESPONSE 상태를 초기화하고 이번 응답을 기다린다. */
    context->heartbeat_response_received = false;
    context->heartbeat_response_sequence = 0;

    pthread_mutex_unlock(&context->heartbeat_mutex);

    if (
        send_heartbeat_packet(
            context,
            &context->go_address,
            BTP_HEARTBEAT_REQUEST,
            current_sequence
        ) < 0
    )
    {
        return false;
    }

    struct timespec deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
    {
        return false;
    }

    add_seconds(&deadline, HEARTBEAT_TIMEOUT_SECONDS);

    pthread_mutex_lock(&context->heartbeat_mutex);

    while (context->running)
    {
        if (
            context->heartbeat_response_received &&
            context->heartbeat_response_sequence ==
                current_sequence
        )
        {
            context->heartbeat_response_received = false;

            pthread_mutex_unlock(
                &context->heartbeat_mutex
            );

            return true;
        }

        /*
         * 기다리는 동안 pthread_cond_timedwait()가 heartbeat_mutex를
         * 자동으로 풀어주므로 Receive Thread가 RESPONSE 상태를 기록할 수 있다.
         * signal을 받아 깨어날 때는 mutex를 다시 획득한 상태로 반환한다.
         */
        int wait_result = pthread_cond_timedwait(
            &context->heartbeat_condition,
            &context->heartbeat_mutex,
            &deadline
        );

        if (wait_result == ETIMEDOUT)
        {
            pthread_mutex_unlock(
                &context->heartbeat_mutex
            );

            return false;
        }

        if (wait_result != 0)
        {
            pthread_mutex_unlock(
                &context->heartbeat_mutex
            );

            return false;
        }
    }

    pthread_mutex_unlock(&context->heartbeat_mutex);
    return false;
}

/*
 * Receiver(Client 역할)의 Heartbeat payload를 만들어 GO로 전송한다.
 *
 * role에는 BTP_ROLE_CLIENT를 넣으며,
 * REQUEST와 RESPONSE 모두 같은 공통 BTP 송신 함수로 전달한다.
 */
static int send_heartbeat_packet(
    receiver_context_t *context,
    const struct sockaddr_in *destination,
    uint32_t packet_type,
    uint32_t sequence
)
{
    btp_heartbeat_payload_t heartbeat;

    heartbeat.magic = htonl(BTP_HEARTBEAT_MAGIC);
    heartbeat.type = htonl(packet_type);
    heartbeat.role = htonl(BTP_ROLE_CLIENT);
    heartbeat.sequence = htonl(sequence);

    return send_btp_payload(
        context,
        destination,
        BTP_PORT_HEARTBEAT,
        (const uint8_t *)&heartbeat,
        sizeof(heartbeat)
    );
}

/*
 * 실제 UDP 송신의 공통 하위 함수다.
 *
 * 순수 payload 앞에 프로젝트의 BTP header를 붙인 뒤 sendto()한다.
 * DENM, ACK, Heartbeat 모두 이 함수로 최종 UDP 패킷이 만들어진다.
 *
 * destination_port는 UDP 포트가 아니라 BTP destination port다.
 * 실제 UDP 전송 포트는 sockaddr_in에 설정된 NETWORK_PORT(5000)이다.
 */
static int send_btp_payload(
    receiver_context_t *context,
    const struct sockaddr_in *destination,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_size
)
{
    uint8_t packet[NETWORK_BUFFER_SIZE];

    size_t packet_size = btp_build_packet(
        packet,
        sizeof(packet),
        destination_port,
        payload,
        payload_size
    );

    if (packet_size == 0)
    {
        return -1;
    }

    ssize_t sent_bytes = sendto(
        context->udp_fd,
        packet,
        packet_size,
        0,
        (const struct sockaddr *)destination,
        sizeof(*destination)
    );

    if (sent_bytes < 0)
    {
        return -1;
    }

    if ((size_t)sent_bytes != packet_size)
    {
        return -1;
    }

    return 0;
}

/* ============================================================
 * 6. 종료 자원 정리
 * ============================================================ */

/*
 * Receiver가 보유한 Filter IPC fd, UDP socket, condition variable, mutex를 정리한다.
 * 모든 Thread가 종료된 뒤 main()에서 호출한다.
 */
static void destroy_context(receiver_context_t *context)
{
    if (context->filter_fd >= 0)
    {
        ipc_close(context->filter_fd);
        context->filter_fd = -1;
    }

    if (context->udp_fd >= 0)
    {
        close(context->udp_fd);
        context->udp_fd = -1;
    }

    pthread_cond_destroy(&context->denm_queue_condition);
    pthread_cond_destroy(&context->monitoring_condition);
    pthread_cond_destroy(&context->heartbeat_condition);

    pthread_mutex_destroy(&context->denm_queue_mutex);
    pthread_mutex_destroy(&context->heartbeat_mutex);
}

/* ============================================================
 * 7. 프로그램 진입점
 * ============================================================ */

/*
 * Receiver 프로그램의 전체 실행 흐름.
 *
 * 1) GO IP를 받아 context 초기화
 * 2) signal 처리 등록
 * 3) DENM Thread 생성
 * 4) Receive Thread 생성
 * 5) Heartbeat Thread 생성
 * 6) Heartbeat Thread가 끝날 때까지 대기
 * 7) 전체 종료 요청
 * 8) Receive / DENM Thread join
 * 9) socket / mutex / condition 정리
 */
/*
 * ============================================================================
 * main() 읽는 순서
 * ============================================================================
 *
 *  1. <go_ip> 실행 인자 검사
 *  2. initialize_context()로 GO 주소 / pthread / UDP socket 준비
 *  3. signal handler 등록
 *  4. DENM Thread 생성
 *  5. Receive Thread 생성
 *  6. Heartbeat Thread 생성
 *  7. main()은 Heartbeat Thread 종료를 기다림
 *  8. 전체 종료 요청
 *  9. Receive / DENM Thread join
 * 10. socket / condition variable / mutex 정리
 *
 * Receiver의 실제 업무는 모두 3개 Thread가 수행하므로
 * main()은 Thread 생성과 종료 순서를 관리하는 역할이 중심이다.
 * ============================================================================
 */
int main(int argc, char *argv[])
{

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    if (argc != 2)
    {
        fprintf(
            stderr,
            "Usage: %s <go_ip>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    receiver_context_t context;

    if (initialize_context(&context, argv[1]) < 0)
    {
        fprintf(
            stderr,
            "Receiver 초기화 실패\n"
        );

        return EXIT_FAILURE;
    }

    static const char *const watchdog_names[] = {"p2p DENM/filter", "p2p UDP", "p2p peer heartbeat"};
    if (ace_watchdog_start(&watchdog, watchdog_names, 3) != 0)
    {
        perror("watchdog start");
        destroy_context(&context);
        return EXIT_FAILURE;
    }
    global_context = &context;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if (
        pthread_create(
            &context.denm_thread,
            NULL,
            denm_thread_main,
            &context
        ) != 0
    )
    {
        fprintf(stderr, "DENM Thread 생성 실패\n");
        request_stop(&context);
        ace_watchdog_stop(&watchdog);
        destroy_context(&context);
        global_context = NULL;
        return EXIT_FAILURE;
    }

    if (
        pthread_create(
            &context.receive_thread,
            NULL,
            receive_thread_main,
            &context
        ) != 0
    )
    {
        fprintf(stderr, "Receive Thread 생성 실패\n");
        request_stop(&context);
        pthread_join(context.denm_thread, NULL);
        ace_watchdog_stop(&watchdog);
        destroy_context(&context);
        global_context = NULL;
        return EXIT_FAILURE;
    }

    if (
        pthread_create(
            &context.heartbeat_thread,
            NULL,
            heartbeat_thread_main,
            &context
        ) != 0
    )
    {
        fprintf(stderr, "Heartbeat Thread 생성 실패\n");
        request_stop(&context);
        pthread_join(context.receive_thread, NULL);
        pthread_join(context.denm_thread, NULL);
        ace_watchdog_stop(&watchdog);
        destroy_context(&context);
        global_context = NULL;
        return EXIT_FAILURE;
    }

    /*
     * main()은 Heartbeat Thread를 "대표 생명주기 Thread"처럼 기다린다.
     * 정상 실행 중에는 Heartbeat Thread가 계속 살아 있고,
     * 장애 판단이나 signal로 running이 0이 되면 Heartbeat Thread가 종료된다.
     */
    pthread_join(context.heartbeat_thread, NULL);

    /*
     * Heartbeat Thread가 끝났다면 다른 blocking 작업들도 종료 방향으로
     * 확실하게 깨워 Receive / DENM Thread가 빠져나오도록 한다.
     */
    request_stop(&context);

    /*
     * 각 Thread가 context, socket, mutex를 더 이상 사용하지 않는 것을
     * 보장한 다음 destroy_context()로 실제 자원을 해제한다.
     */
    pthread_join(context.receive_thread, NULL);
    pthread_join(context.denm_thread, NULL);

    ace_watchdog_stop(&watchdog);
    destroy_context(&context);
    global_context = NULL;

    return EXIT_SUCCESS;
}
