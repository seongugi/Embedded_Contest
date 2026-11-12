
/*
 * ============================================================================
 * broadcast_sender.c
 * ============================================================================
 *
 * [이 파일의 역할]
 *
 * Wi-Fi Direct 그룹의 GO(Group Owner) 쪽에서 동작하는 Sender 프로세스다.
 *
 * 이 프로세스는 크게 3개의 실행 흐름(Thread)을 가진다.
 *
 *   1) Main Thread
 *      - Unix Domain Socket IPC(/tmp/p2p.sock)의 서버가 된다.
 *      - 다른 프로세스가 전달한 IPC_MSG_ACCIDENT_EVENT를 받는다.
 *      - accident_ipc_event_t를 전체 accident_event_payload_t로 확장한다.
 *      - accident_event_payload_t를 DENM_t로 변환한다.
 *      - DENM을 ASN.1 UPER로 인코딩한다.
 *      - BTP_PORT_DENM을 붙여 UDP Broadcast한다.
 *      - 같은 sequence의 DENM ACK가 모든 Client에서 올 때까지 기다린다.
 *
 *   2) Receive Thread
 *      - 이 프로세스에서 UDP recvfrom()을 전담한다.
 *      - UDP 5000번으로 들어온 BTP 패킷을 파싱한다.
 *      - HEARTBEAT 패킷이면 Heartbeat 처리 루틴으로 넘긴다.
 *      - ACK 패킷이면 Main Thread가 기다리는 ACK 상태를 갱신한다.
 *
 *   3) Heartbeat Thread
 *      - 실행 인자로 받은 Client들을 한 대씩 순차적으로 검사한다.
 *      - Client에게 HEARTBEAT REQUEST를 보내고 RESPONSE를 최대 1초 기다린다.
 *      - 한 Client가 연속 3회 응답하지 않으면 장애로 판단하고 전체 종료를 요청한다.
 *
 *
 * [가장 중요한 설계 원칙]
 *
 *   ★ UDP recvfrom()은 Receive Thread 한 곳에서만 호출한다.
 *
 * Heartbeat Thread나 Main Thread가 각각 recvfrom()을 호출하면
 * "어느 Thread가 어떤 UDP 패킷을 먼저 가져갈지" 경쟁이 발생할 수 있다.
 * 따라서 Receive Thread가 모든 UDP 패킷을 받은 뒤 BTP destination port를 보고
 * 필요한 Thread에 condition variable을 통해 결과만 전달한다.
 *
 *
 * [Thread 사이 데이터 전달]
 *
 *   Receive Thread ── Heartbeat RESPONSE ──> Heartbeat Thread
 *                    heartbeat_mutex
 *                    heartbeat_condition
 *
 *   Receive Thread ── DENM ACK ───────────> Main Thread
 *                    ack_mutex
 *                    ack_condition
 *
 *
 * [DENM 송신 흐름]
 *
 *   IPC_MSG_ACCIDENT_EVENT
 *            |
 *            v
 *   accident_ipc_event_t
 *            |
 *            v
 *   accident_event_payload_t
 *            |
 *            | fill_denm()
 *            v
 *          DENM_t
 *            |
 *            | encode_denm()
 *            v
 *      UPER encoded bytes
 *            |
 *            | btp_build_packet()
 *            v
 *      [BTP header + DENM]
 *            |
 *            | sendto()
 *            v
 *   192.168.49.255:5000 Broadcast
 *            |
 *            v
 *        Client ACK
 *            |
 *            v
 *      Receive Thread
 *            |
 *            v
 *        Main Thread
 *
 *
 * [Heartbeat 흐름]
 *
 *   Heartbeat Thread
 *        |
 *        | REQUEST(seq=N)
 *        v
 *      Client
 *        |
 *        | RESPONSE(seq=N)
 *        v
 *   Receive Thread
 *        |
 *        | heartbeat 상태 기록 + condition broadcast
 *        v
 *   Heartbeat Thread 깨어남
 *
 *
 * [UDP 포트와 BTP 포트의 차이]
 *
 * 실제 UDP socket은 하나이며 UDP 포트는 NETWORK_PORT(5000) 하나를 쓴다.
 * DENM / ACK / HEARTBEAT / CAM의 논리적 구분은 BTP destination port가 담당한다.
 *
 *   UDP 5000
 *      ├─ BTP_PORT_DENM
 *      ├─ BTP_PORT_ACK
 *      ├─ BTP_PORT_HEARTBEAT
 *      └─ BTP_PORT_CAM
 *
 * ============================================================================
 */
#include "ipc.h"
#include "runtime_watchdog.h"
#include "protocol.h"
#include "accident_protocol.h"
#include "p2p_protocol.h"
#include "btp.h"

#include "denm_builder.h"
#include "denm_encoder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "DENM.h"
#include "DeltaAltitude.h"
#include "DeltaLatitude.h"
#include "DeltaLongitude.h"
#include "Latitude.h"
#include "Longitude.h"
#include "SpeedValue.h"

/*
 * =========================
 * 네트워크 / IPC 기본 설정
 * =========================
 *
 * 실제 UDP 송수신은 하나의 NETWORK_PORT(5000)를 사용한다.
 * 패킷의 논리적 종류(DENM/ACK/Heartbeat/CAM)는 BTP destination port로 구분한다.
 */
#define NETWORK_PORT 5000
#define BROADCAST_IP "192.168.49.255"
#define P2P_IPC_SOCKET_PATH "/tmp/p2p.sock"

#define NETWORK_BUFFER_SIZE 4096
#define IPC_PAYLOAD_SIZE 1024
#define MAX_PEERS 32

#define DENM_ACK_TIMEOUT_SECONDS 3
#define DENM_RETRY_AFTER_SECONDS 1
#define HEARTBEAT_INTERVAL_SECONDS 2
#define HEARTBEAT_TIMEOUT_SECONDS 1
#define MAX_CONSECUTIVE_FAILURES 3

enum { WD_IPC, WD_RECEIVE, WD_HEARTBEAT };
static ace_watchdog_t watchdog;

/* Idle sockets wake once a second; work deadlines include the existing ACK wait. */
#define WD_IDLE_MS 5000U
#define WD_IPC_FRAME_MS 8000U
#define WD_DENM_SEND_MS 12000U

#define ITS_EPOCH_UNIX_MS 1072915200000ULL
#define ITS_LEAP_SECONDS_MS 5000ULL

/*
 * Heartbeat와 ACK 관리에 필요한 Client 한 대의 정보.
 * address는 Heartbeat unicast 전송에 사용되고 failure_count는 연속 응답 실패 횟수다.
 */
typedef struct
{
    char ip[INET_ADDRSTRLEN];
    struct sockaddr_in address;
    int failure_count;

} peer_t;

/*
 * Sender 프로세스 전체가 공유하는 실행 context.
 *
 * Main / Receive / Heartbeat Thread가 이 구조체 하나를 공유한다.
 * 공유 상태 중 Heartbeat 관련 필드는 heartbeat_mutex,
 * ACK 관련 필드는 ack_mutex로 보호한다.
 */
typedef struct
{
    /* UDP/BTP socket, IPC server socket, 현재 연결된 IPC client socket */
    int udp_fd;
    int server_fd;
    int ipc_fd;

    /* DENM broadcast 목적지(192.168.49.255:5000) */
    struct sockaddr_in broadcast_address;

    /* 실행 인자로 받은 Client 목록과 DENM ACK 목표 개수 */
    peer_t peers[MAX_PEERS];
    int peer_count;
    int expected_ack_count;

    /* 1이면 실행, 0이면 모든 Thread가 종료 방향으로 진행한다. */
    volatile sig_atomic_t running;

    /* UDP 수신 전담 Thread와 능동 Heartbeat 감시 Thread */
    pthread_t heartbeat_thread;
    pthread_t receive_thread;

    /*
     * Receive Thread가 기록한 Heartbeat RESPONSE를
     * Heartbeat Thread가 condition variable로 기다린다.
     */
    pthread_mutex_t heartbeat_mutex;
    pthread_cond_t heartbeat_condition;

    uint32_t heartbeat_sequence;
    bool heartbeat_response_received;
    uint32_t heartbeat_response_sequence;
    char heartbeat_response_ip[INET_ADDRSTRLEN];

    /*
     * Receive Thread가 기록한 DENM ACK 상태를
     * Main Thread가 condition variable로 기다린다.
     */
    pthread_mutex_t ack_mutex;
    pthread_cond_t ack_condition;

    bool ack_waiting;
    uint32_t ack_sequence;
    bool ack_received[MAX_PEERS];
    int ack_received_count;

} sender_context_t;

/*
 * signal_handler에서 현재 실행 context에 접근하기 위한 전역 포인터.
 * 정상 실행 로직에서는 가능한 한 지역 context 포인터를 사용한다.
 */
static sender_context_t *global_context = NULL;

/* ============================================================
 * 함수 원형 선언
 *
 * 실제 구현은 아래에서 main()의 실행 흐름에 맞춰 배치한다.
 * ============================================================ */

static uint64_t get_timestamp_its_ms(void);

static void add_seconds(
    struct timespec *time_value,
    time_t seconds
);

static int parse_positive_int(
    const char *text,
    int *value_out
);

static int initialize_peer(
    peer_t *peer,
    const char *ip
);

static int initialize_sync_objects(sender_context_t *context);

static int create_udp_socket(void);

static void signal_handler(int signal_number);

static void request_stop(sender_context_t *context);

static void *receive_thread_main(void *argument);

static void handle_heartbeat_payload(
    sender_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct sockaddr_in *sender_address,
    const char *sender_ip
);

static void record_heartbeat_response(
    sender_context_t *context,
    const char *sender_ip,
    uint32_t sequence
);

static void handle_ack_payload(
    sender_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const char *sender_ip
);

static int find_peer_index(
    const sender_context_t *context,
    const char *ip
);

static void *heartbeat_thread_main(void *argument);

static bool check_peer(
    sender_context_t *context,
    const peer_t *peer
);

static int send_heartbeat_packet(
    sender_context_t *context,
    const struct sockaddr_in *destination,
    uint32_t packet_type,
    uint32_t sequence
);

static int send_btp_payload(
    sender_context_t *context,
    const struct sockaddr_in *destination,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_size,
    int send_flags
);

static int send_denm_event(
    sender_context_t *context,
    const accident_event_payload_t *event
);

static void prepare_ack_wait(
    sender_context_t *context,
    uint32_t sequence
);

static int wait_for_denm_acks(
    sender_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct timespec *sent_at
);

static void cancel_ack_wait(sender_context_t *context);

static void destroy_sync_objects(sender_context_t *context);

static int wait_for_ipc_input(sender_context_t *context, int fd)
{
    struct pollfd input = {.fd = fd, .events = POLLIN};
    while (context->running)
    {
        ace_watchdog_progress(&watchdog, WD_IPC, WD_IDLE_MS);
        int result = poll(&input, 1, 1000);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (result > 0 && (input.revents & (POLLERR | POLLNVAL))))
            return -1;
        if (result > 0 && (input.revents & (POLLIN | POLLHUP))) return 0;
    }
    return -1;
}

/* ============================================================
 * 공통 보조 함수
 * ============================================================ */

/*
 * 현재 CLOCK_REALTIME 시각을 DENM TimestampIts 형식의 밀리초로 변환한다.
 *
 * CLOCK_REALTIME은 Unix epoch(1970-01-01)를 기준으로 하므로
 * ITS epoch(2004-01-01)까지의 차이를 빼고,
 * ITS epoch 이후 발생한 윤초 누적값을 더한다.
 *
 * Sender에서는 DENM을 생성하는 현재 시각을
 * Management Container의 referenceTime으로 기록할 때 사용한다.
 */
static uint64_t get_timestamp_its_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
    {
        return 0;
    }

    uint64_t unix_ms =
        (uint64_t)now.tv_sec * 1000ULL +
        (uint64_t)now.tv_nsec / 1000000ULL;

    if (unix_ms < ITS_EPOCH_UNIX_MS)
    {
        return 0;
    }

    return
        unix_ms
        - ITS_EPOCH_UNIX_MS
        + ITS_LEAP_SECONDS_MS;
}

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

/* ============================================================
 * 1. 명령행 인자 및 초기화
 * ============================================================ */

/*
 * 문자열로 전달된 expected_ack_count를 안전하게 정수로 변환한다.
 *
 * main()이 argv[1]을 해석할 때 사용한다.
 * 1~MAX_PEERS 범위만 허용하며 숫자가 아닌 문자가 섞였거나,
 * 오버플로가 발생했거나, 0 이하라면 실패로 처리한다.
 */
static int parse_positive_int(
    const char *text,
    int *value_out
)
{
    char *end;
    long value;

    /*
     * strtol()은 atoi()와 달리 변환 실패 / 범위 오류 / 뒤에 이상한 문자가
     * 붙었는지를 검사할 수 있다.
     *
     * 예)
     *   "2"     -> 성공
     *   "0"     -> 아래 범위 검사에서 실패
     *   "-1"    -> 실패
     *   "abc"   -> end == text
     *   "2abc"  -> *end != '\0'
     */
    errno = 0;
    value = strtol(text, &end, 10);

    if (
        errno != 0 ||
        end == text ||
        *end != '\0' ||
        value <= 0 ||
        value > MAX_PEERS
    )
    {
        return -1;
    }

    *value_out = (int)value;
    return 0;
}

/*
 * Client 한 대의 Heartbeat 대상 정보를 초기화한다.
 *
 * 문자열 IP를 peer->ip에 보관하고 sockaddr_in 형태로도 변환해 둔다.
 * 이후 Heartbeat Thread가 매번 주소를 다시 만들지 않고 바로 sendto()
 * 할 수 있도록 초기화 단계에서 준비한다.
 */
static int initialize_peer(
    peer_t *peer,
    const char *ip
)
{
    memset(peer, 0, sizeof(*peer));

    if (strlen(ip) >= sizeof(peer->ip))
    {
        return -1;
    }

    strcpy(peer->ip, ip);

    peer->address.sin_family = AF_INET;
    peer->address.sin_port = htons(NETWORK_PORT);

    if (
        inet_pton(
            AF_INET,
            ip,
            &peer->address.sin_addr
        ) != 1
    )
    {
        return -1;
    }

    peer->failure_count = 0;
    return 0;
}

/*
 * Sender가 사용하는 mutex / condition variable을 초기화한다.
 *
 * heartbeat_mutex + heartbeat_condition:
 *   Heartbeat RESPONSE 상태를 Receive Thread와 Heartbeat Thread가 공유한다.
 *
 * ack_mutex + ack_condition:
 *   DENM ACK 수신 상태를 Receive Thread와 Main Thread가 공유한다.
 *
 * condition variable의 clock은 CLOCK_MONOTONIC으로 맞춘다.
 * 시스템 시간이 변경되어도 timeout 계산이 흔들리지 않게 하기 위함이다.
 */
static int initialize_sync_objects(sender_context_t *context)
{
    if (pthread_mutex_init(&context->heartbeat_mutex, NULL) != 0)
    {
        return -1;
    }

    if (pthread_mutex_init(&context->ack_mutex, NULL) != 0)
    {
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    pthread_condattr_t condition_attribute;

    if (pthread_condattr_init(&condition_attribute) != 0)
    {
        pthread_mutex_destroy(&context->ack_mutex);
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
        pthread_mutex_destroy(&context->ack_mutex);
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
        pthread_mutex_destroy(&context->ack_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    if (
        pthread_cond_init(
            &context->ack_condition,
            &condition_attribute
        ) != 0
    )
    {
        pthread_cond_destroy(&context->heartbeat_condition);
        pthread_condattr_destroy(&condition_attribute);
        pthread_mutex_destroy(&context->ack_mutex);
        pthread_mutex_destroy(&context->heartbeat_mutex);
        return -1;
    }

    pthread_condattr_destroy(&condition_attribute);
    return 0;
}

/*
 * UDP/BTP 송수신에 공통으로 사용할 UDP socket을 생성하고 5000번 포트에 bind한다.
 *
 * SO_REUSEADDR:
 *   프로세스 재시작 시 같은 포트를 다시 bind하기 쉽게 한다.
 *
 * SO_BROADCAST:
 *   192.168.49.255 같은 broadcast 주소로 DENM을 보낼 수 있게 한다.
 *
 * 이 socket 하나를 Receive Thread, Heartbeat Thread, Main Thread가 함께 사용한다.
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

    if (
        setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_BROADCAST,
            &enable,
            sizeof(enable)
        ) < 0
    )
    {
        perror("setsockopt SO_BROADCAST");
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
 * SIGINT / SIGTERM 수신 시 blocking I/O를 깨우기 위한 최소 종료 처리다.
 *
 * signal handler 안에서는 복잡한 pthread 동기화 처리를 하지 않고,
 * running을 0으로 만들고 socket을 shutdown()하여
 * recvfrom()/IPC 대기 중인 흐름이 빠져나올 수 있게 한다.
 */
static void signal_handler(int signal_number)
{
    (void)signal_number;

    if (global_context == NULL)
    {
        return;
    }

    global_context->running = 0;

    if (global_context->udp_fd >= 0)
    {
        shutdown(global_context->udp_fd, SHUT_RDWR);
    }

    if (global_context->ipc_fd >= 0)
    {
        shutdown(global_context->ipc_fd, SHUT_RDWR);
    }

    if (global_context->server_fd >= 0)
    {
        shutdown(global_context->server_fd, SHUT_RDWR);
    }
}

/*
 * 일반 실행 흐름에서 Sender 전체 중지를 요청한다.
 *
 * running을 0으로 바꾸고 condition variable을 broadcast하여
 * pthread_cond_timedwait() 중인 Thread들을 깨운 뒤,
 * UDP/IPC socket도 shutdown()해서 blocking I/O를 종료시킨다.
 *
 * signal_handler와 달리 일반 함수이므로 mutex/condition을 안전하게 사용할 수 있다.
 */
static void request_stop(sender_context_t *context)
{
    if (!context->running)
    {
        return;
    }

    context->running = 0;

    pthread_mutex_lock(&context->heartbeat_mutex);
    pthread_cond_broadcast(&context->heartbeat_condition);
    pthread_mutex_unlock(&context->heartbeat_mutex);

    pthread_mutex_lock(&context->ack_mutex);
        /*
         * Main Thread가 wait_for_denm_acks() 안에서 잠들어 있을 수 있다.
         * ACK 상태가 바뀌었음을 알려 조건을 다시 검사하게 만든다.
         */
        pthread_cond_broadcast(&context->ack_condition);
    pthread_mutex_unlock(&context->ack_mutex);

    if (context->udp_fd >= 0)
    {
        shutdown(context->udp_fd, SHUT_RDWR);
    }

    if (context->ipc_fd >= 0)
    {
        shutdown(context->ipc_fd, SHUT_RDWR);
    }

    if (context->server_fd >= 0)
    {
        shutdown(context->server_fd, SHUT_RDWR);
    }
}

/* ============================================================
 * 3. Receive Thread - UDP 수신 및 분배
 * ============================================================ */

/*
 * [Receive Thread]
 * UDP 5000번 포트의 수신을 전담한다.
 *
 * 이 Thread가 recvfrom()을 한 곳에서만 수행한 뒤 BTP destination port를 보고
 * Heartbeat / ACK / DENM / CAM으로 분배한다.
 *
 * Sender 측에서는
 *   - HEARTBEAT: 요청에 응답하거나 RESPONSE를 Heartbeat Thread에 전달
 *   - ACK      : DENM ACK를 Main Thread에 전달
 * 를 처리한다.
 *
 * 실제 ACK 대기와 Heartbeat timeout 대기는 각각 다른 Thread가 condition variable로
 * 수행하므로 Receive Thread는 "수신 + 분배"에 집중한다.
 */
static void *receive_thread_main(void *argument)
{
    sender_context_t *context =
        (sender_context_t *)argument;

    uint8_t receive_buffer[NETWORK_BUFFER_SIZE];

    while (context->running)
    {
        ace_watchdog_progress(&watchdog, WD_RECEIVE, WD_IDLE_MS);
        struct sockaddr_in sender_address;
        socklen_t sender_address_length =
            sizeof(sender_address);

            /*
         * ★ Sender의 UDP recvfrom()은 이 Receive Thread만 수행한다.
         *
         * Main Thread와 Heartbeat Thread가 직접 recvfrom() 하지 않기 때문에
         * ACK과 Heartbeat RESPONSE가 서로 다른 Thread에 잘못 소비되는 문제를 피한다.
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

        /*
         * 실제 UDP 포트는 모두 5000이지만,
         * BTP header 안의 destination_port로 논리적 메시지 종류를 구분한다.
         *
         * 여기서 Receive Thread는 "dispatcher" 역할을 한다.
         */
        switch (destination_port)
        {
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
            handle_ack_payload(
                context,
                payload,
                payload_size,
                sender_ip
            );
            break;

        case BTP_PORT_DENM:
            /* GO Sender에서는 수신 DENM을 처리하지 않는다. */
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
 * 수신한 BTP Heartbeat payload를 검사하고 REQUEST/RESPONSE를 처리한다.
 *
 * REQUEST:
 *   상대가 Sender(GO)의 생존 여부를 물은 것이므로 같은 sequence로 RESPONSE를 보낸다.
 *
 * RESPONSE:
 *   Heartbeat Thread가 기다리던 응답일 수 있으므로
 *   record_heartbeat_response()에 기록하고 condition variable을 깨운다.
 */
static void handle_heartbeat_payload(
    sender_context_t *context,
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
    uint32_t sequence = ntohl(heartbeat.sequence);

    if (magic != BTP_HEARTBEAT_MAGIC)
    {
        return;
    }

    if (packet_type == BTP_HEARTBEAT_REQUEST)
    {
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
                "[Heartbeat] %s 응답 전송 실패\n",
                sender_ip
            );
        }
    }
    else if (packet_type == BTP_HEARTBEAT_RESPONSE)
    {
        record_heartbeat_response(
            context,
            sender_ip,
            sequence
        );
    }
}

/*
 * Receive Thread가 받은 Heartbeat RESPONSE 정보를 공유 상태에 기록한다.
 *
 * Heartbeat Thread는 heartbeat_condition에서 잠들어 있으므로,
 * 응답 sequence와 송신 IP를 기록한 뒤 pthread_cond_broadcast()로 깨운다.
 * IP까지 함께 기록하는 이유는 여러 Client를 순차 검사할 때
 * 다른 Client의 응답을 현재 대상의 응답으로 오인하지 않기 위해서다.
 */
static void record_heartbeat_response(
    sender_context_t *context,
    const char *sender_ip,
    uint32_t sequence
)
{
    pthread_mutex_lock(&context->heartbeat_mutex);

    context->heartbeat_response_received = true;
    context->heartbeat_response_sequence = sequence;

    strncpy(
        context->heartbeat_response_ip,
        sender_ip,
        sizeof(context->heartbeat_response_ip) - 1
    );

    context->heartbeat_response_ip[
        sizeof(context->heartbeat_response_ip) - 1
    ] = '\0';

    pthread_cond_broadcast(&context->heartbeat_condition);

    pthread_mutex_unlock(&context->heartbeat_mutex);
}

/*
 * Client가 보낸 DENM ACK를 처리한다.
 *
 * 현재 ACK 대기 중인지, sequence가 현재 DENM과 일치하는지,
 * 같은 Client의 중복 ACK가 아닌지를 모두 확인한 뒤 ACK 개수를 증가시킨다.
 *
 * 조건을 만족하면 ack_condition을 깨워 Main Thread의 wait_for_denm_acks()가
 * 즉시 상태를 다시 확인하도록 한다.
 */
static void handle_ack_payload(
    sender_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const char *sender_ip
)
{
    btp_ack_payload_t ack;

    if (payload_size != sizeof(ack))
    {
        return;
    }

    memcpy(&ack, payload, sizeof(ack));

    uint32_t sequence = ntohl(ack.sequence);
    int peer_index = find_peer_index(context, sender_ip);

    if (peer_index < 0)
    {
        return;
    }

    pthread_mutex_lock(&context->ack_mutex);

    if (
        context->ack_waiting &&
        context->ack_sequence == sequence &&
        !context->ack_received[peer_index]
    )
    {
        /*
         * ACK는 Client IP별로 한 번만 인정한다.
         * ack_received[peer_index]가 이미 true라면 동일 Client의
         * 중복 ACK는 위 조건에서 걸러져 count가 증가하지 않는다.
         */
        context->ack_received[peer_index] = true;
        context->ack_received_count++;

        printf(
            "[ACK] %s 수신 (sequence=%u, %d/%d)\n",
            sender_ip,
            sequence,
            context->ack_received_count,
            context->expected_ack_count
        );

        pthread_cond_broadcast(&context->ack_condition);
    }

    pthread_mutex_unlock(&context->ack_mutex);
}

/*
 * ACK 송신 IP가 등록된 Client 목록의 몇 번째 항목인지 찾는다.
 *
 * ACK 중복 제거용 ack_received[]의 인덱스를 얻기 위해 사용한다.
 * 등록되지 않은 IP라면 -1을 반환하고 ACK는 무시한다.
 */
static int find_peer_index(
    const sender_context_t *context,
    const char *ip
)
{
    for (int i = 0; i < context->peer_count; i++)
    {
        if (strcmp(context->peers[i].ip, ip) == 0)
        {
            return i;
        }
    }

    return -1;
}

/* ============================================================
 * 4. Heartbeat Thread - Client 생존 감시
 * ============================================================ */

/*
 * [Heartbeat Thread]
 * 등록된 Client들을 순서대로 능동 감시한다.
 *
 * 각 Client마다 check_peer()로 REQUEST/RESPONSE 확인을 수행하고,
 * 성공하면 failure_count를 0으로 초기화한다.
 * 실패가 MAX_CONSECUTIVE_FAILURES 회 연속 발생하면 해당 Client 장애로 판단하고
 * request_stop()으로 Sender 전체를 종료한다.
 *
 * 한 바퀴 검사 후 HEARTBEAT_INTERVAL_SECONDS 만큼 기다렸다가 다시 반복한다.
 */
static void *heartbeat_thread_main(void *argument)
{
    sender_context_t *context =
        (sender_context_t *)argument;

    while (context->running)
    {
        ace_watchdog_progress(&watchdog, WD_HEARTBEAT, WD_IDLE_MS);
        for (int i = 0; i < context->peer_count; i++)
        {
            ace_watchdog_progress(&watchdog, WD_HEARTBEAT, WD_IDLE_MS);
            peer_t *peer = &context->peers[i];

            if (!context->running)
            {
                break;
            }

            if (check_peer(context, peer))
            {
                peer->failure_count = 0;

            }
            else
            {
                if (!context->running)
                {
                    break;
                }

                peer->failure_count++;

                printf(
                    "[Heartbeat] %s 응답 없음 (%d/%d)\n",
                    peer->ip,
                    peer->failure_count,
                    MAX_CONSECUTIVE_FAILURES
                );

                if (
                    peer->failure_count >=
                    MAX_CONSECUTIVE_FAILURES
                )
                {
                    printf(
                        "[Heartbeat] %s 장애 판단\n",
                        peer->ip
                    );

                    request_stop(context);
                    return NULL;
                }
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
 * Client 한 대에 Heartbeat REQUEST를 보내고 RESPONSE를 timeout 동안 기다린다.
 *
 * 1) 새 sequence를 발급하고 이전 응답 상태를 초기화
 * 2) REQUEST 전송
 * 3) CLOCK_MONOTONIC 기준 deadline 계산
 * 4) heartbeat_condition에서 대기
 * 5) 같은 sequence + 같은 Client IP 응답이면 성공
 *
 * Receive Thread가 RESPONSE를 받으면 공유 상태를 기록하고 condition을 깨운다.
 */
static bool check_peer(
    sender_context_t *context,
    const peer_t *peer
)
{
    uint32_t current_sequence;

    pthread_mutex_lock(&context->heartbeat_mutex);

    /*
     * Heartbeat RESPONSE는 Receive Thread가 기록하므로
     * 이 공유 상태를 건드릴 때는 heartbeat_mutex를 반드시 잡는다.
     *
     * 새로운 검사마다 sequence를 1 증가시켜
     * "예전에 늦게 도착한 RESPONSE"를 현재 검사 결과로 착각하지 않게 한다.
     */
    context->heartbeat_sequence++;
    current_sequence = context->heartbeat_sequence;

    /*
     * 직전 검사 결과를 제거한다.
     * 이후 Receive Thread가 이번 sequence에 맞는 RESPONSE를 받으면
     * 아래 필드들이 다시 채워진다.
     */
    context->heartbeat_response_received = false;
    context->heartbeat_response_sequence = 0;
    context->heartbeat_response_ip[0] = '\0';

    pthread_mutex_unlock(&context->heartbeat_mutex);

    if (
        send_heartbeat_packet(
            context,
            &peer->address,
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
            context->heartbeat_response_sequence == current_sequence &&
            strcmp(
                context->heartbeat_response_ip,
                peer->ip
            ) == 0
        )
        {
            context->heartbeat_response_received = false;

            pthread_mutex_unlock(&context->heartbeat_mutex);
            return true;
        }

            /*
         * ★ pthread_cond_timedwait()의 핵심
         *
         * 호출 시 heartbeat_mutex를 잡고 있지만,
         * 실제로 잠드는 동안에는 함수가 mutex를 자동으로 풀어준다.
         * 따라서 Receive Thread가 같은 heartbeat_mutex를 잡고
         * RESPONSE 정보를 기록할 수 있다.
         *
         * Receive Thread가 pthread_cond_broadcast()를 호출하면 깨어나며,
         * 깨어나서 이 함수가 반환되기 직전에 mutex를 다시 획득한다.
         *
         * 즉:
         *
         *   Heartbeat Thread : mutex 잠금 -> cond_wait -> [잠자는 동안 mutex 해제]
         *   Receive Thread   : mutex 획득 -> RESPONSE 기록 -> signal -> mutex 해제
         *   Heartbeat Thread : 깨어남 -> mutex 재획득 -> 조건 재검사
         */
        int wait_result = pthread_cond_timedwait(
            &context->heartbeat_condition,
            &context->heartbeat_mutex,
            &deadline
        );

        if (wait_result == ETIMEDOUT)
        {
            pthread_mutex_unlock(&context->heartbeat_mutex);
            return false;
        }

        if (wait_result != 0)
        {
            pthread_mutex_unlock(&context->heartbeat_mutex);
            return false;
        }
    }

    pthread_mutex_unlock(&context->heartbeat_mutex);
    return false;
}

/*
 * Heartbeat 전용 payload를 구성해 BTP_PORT_HEARTBEAT으로 전송한다.
 *
 * multi-byte 정수는 네트워크 바이트 순서로 변환한다.
 * Sender는 GO 역할이므로 role 필드에 BTP_ROLE_GO를 넣는다.
 */
static int send_heartbeat_packet(
    sender_context_t *context,
    const struct sockaddr_in *destination,
    uint32_t packet_type,
    uint32_t sequence
)
{
    btp_heartbeat_payload_t heartbeat;

    heartbeat.magic = htonl(BTP_HEARTBEAT_MAGIC);
    heartbeat.type = htonl(packet_type);
    heartbeat.role = htonl(BTP_ROLE_GO);
    heartbeat.sequence = htonl(sequence);

    return send_btp_payload(
        context,
        destination,
        BTP_PORT_HEARTBEAT,
        (const uint8_t *)&heartbeat,
        sizeof(heartbeat),
        0
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
    sender_context_t *context,
    const struct sockaddr_in *destination,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_size,
    int send_flags
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
        send_flags,
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
 * 5. Main Thread - DENM 생성 / 송신 / ACK 대기
 * ============================================================ */

/*
 * [Main Thread - DENM 송신 핵심]
 * IPC로 받은 사고 이벤트를 DENM으로 변환하고 broadcast한 뒤 ACK를 기다린다.
 *
 * 처리 흐름:
 *   accident_event_payload_t
 *       -> fill_denm()
 *       -> encode_denm() / UPER
 *       -> BTP_PORT_DENM
 *       -> 192.168.49.255:5000 broadcast
 *       -> wait_for_denm_acks()
 *
 * ACK 준비를 sendto()보다 먼저 하는 이유는,
 * 매우 빠르게 도착한 ACK가 대기 상태 설정 전에 처리되어 유실되는 race를 막기 위해서다.
 */
static int send_denm_event(
    sender_context_t *context,
    const accident_event_payload_t *event
)
{
    DENM_t *denm = calloc(1, sizeof(*denm));

    if (denm == NULL)
    {
        perror("calloc");
        return -1;
    }

    if (
        fill_denm(
            denm,
            event
        ) != 0
    )
    {
        ASN_STRUCT_FREE(asn_DEF_DENM, denm);
        return -1;
    }

    uint8_t encoded_buffer[NETWORK_BUFFER_SIZE];
    size_t encoded_bytes;
    ssize_t encoded_bits;

    if (
        encode_denm(
            denm,
            encoded_buffer,
            sizeof(encoded_buffer),
            &encoded_bytes,
            &encoded_bits
        ) != 0
    )
    {
        ASN_STRUCT_FREE(asn_DEF_DENM, denm);
        return -1;
    }

    /*
     * ★ ACK 상태를 DENM 전송보다 먼저 준비한다.
     * 빠른 Client가 sendto() 직후 ACK을 보내도 놓치지 않기 위해서다.
     */
    struct timespec sent_at;
    if (clock_gettime(CLOCK_MONOTONIC, &sent_at) < 0)
    {
        perror("clock_gettime");
        ASN_STRUCT_FREE(asn_DEF_DENM, denm);
        return -1;
    }

    prepare_ack_wait(
        context,
        event->sequence_number
    );

    /*
     * 실제 네트워크 계층:
     *   encoded_buffer(UPER DENM)
     *      -> BTP_PORT_DENM header 부착
     *      -> UDP 192.168.49.255:5000 Broadcast
     */
    if (
        send_btp_payload(
            context,
            &context->broadcast_address,
            BTP_PORT_DENM,
            encoded_buffer,
            encoded_bytes,
            0
        ) < 0
    )
    {
        perror("DENM sendto");
        cancel_ack_wait(context);
        ASN_STRUCT_FREE(asn_DEF_DENM, denm);
        return -1;
    }

    printf(
        "DENM Broadcast 완료 (sequence=%u)\n",
        (unsigned int)event->sequence_number
    );

    ASN_STRUCT_FREE(asn_DEF_DENM, denm);

    return wait_for_denm_acks(context, encoded_buffer, encoded_bytes, &sent_at);
}

/*
 * 새 DENM 송신 직전에 ACK 추적 상태를 초기화한다.
 *
 * 현재 sequence를 저장하고 모든 Client의 ack_received[]를 false로 만들며,
 * ack_waiting을 true로 바꾼다.
 */
static void prepare_ack_wait(
    sender_context_t *context,
    uint32_t sequence
)
{
    pthread_mutex_lock(&context->ack_mutex);

    /*
     * DENM을 실제 sendto() 하기 전에 ACK 대기 상태를 먼저 준비한다.
     *
     * 순서를 반대로 하면:
     *   1) DENM 전송
     *   2) Client가 매우 빠르게 ACK 전송
     *   3) Receive Thread가 ACK 수신
     *   4) 아직 ack_waiting == false라 ACK 무시
     *   5) 그 뒤 ACK 대기 시작
     *
     * 같은 race condition이 생길 수 있다.
     *
     * 따라서 반드시 "ACK 받을 준비 -> DENM 전송" 순서를 사용한다.
     */
    context->ack_waiting = true;
    context->ack_sequence = sequence;
    context->ack_received_count = 0;

    memset(
        context->ack_received,
        0,
        sizeof(context->ack_received)
    );

    pthread_mutex_unlock(&context->ack_mutex);
}

/*
 * 현재 DENM에 대한 ACK가 expected_ack_count만큼 들어올 때까지 기다린다.
 *
 * Receive Thread가 ACK를 받을 때마다 ack_received_count를 증가시키고
 * ack_condition을 깨운다. 이 함수는 그 조건을 다시 검사한다.
 *
 * 첫 송신 1초 뒤 ACK가 부족하면 같은 DENM을 한 번 재전송한다.
 * 받은 ACK는 유지하며 전체 대기는 첫 송신부터 3초로 제한한다.
 */
static int wait_for_denm_acks(
    sender_context_t *context,
    const uint8_t *payload,
    size_t payload_size,
    const struct timespec *sent_at
)
{
    struct timespec deadline = *sent_at;
    struct timespec retry_at = *sent_at;
    bool retried = false;
    int result = -1;
    add_seconds(&deadline, DENM_ACK_TIMEOUT_SECONDS);
    add_seconds(&retry_at, DENM_RETRY_AFTER_SECONDS);

    pthread_mutex_lock(&context->ack_mutex);

    printf(
        "Waiting for %d DENM ACKs...\n",
        context->expected_ack_count
    );

    while (
        context->running &&
        context->ack_received_count <
            context->expected_ack_count
    )
    {
        int wait_result = pthread_cond_timedwait(
            &context->ack_condition,
            &context->ack_mutex,
            retried ? &deadline : &retry_at
        );

        /* 타임아웃과 동시에 들어온 ACK 또는 종료 요청도 먼저 반영한다. */
        if (!context->running ||
            context->ack_received_count >= context->expected_ack_count)
        {
            break;
        }

        if (wait_result == ETIMEDOUT)
        {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            {
                perror("clock_gettime");
                break;
            }
            if (!retried &&
                (now.tv_sec < deadline.tv_sec ||
                 (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)))
            {
                retried = true;
                printf("DENM ACK 부족: 1회 재전송 (sequence=%u, %d/%d)\n",
                       (unsigned int)context->ack_sequence,
                       context->ack_received_count, context->expected_ack_count);

                /* 송신 중에도 Receive Thread가 ACK를 기록할 수 있게 한다. */
                pthread_mutex_unlock(&context->ack_mutex);
                if (send_btp_payload(context, &context->broadcast_address,
                                     BTP_PORT_DENM, payload, payload_size,
                                     MSG_DONTWAIT) < 0)
                {
                    perror("DENM 재전송 sendto");
                }
                pthread_mutex_lock(&context->ack_mutex);
                continue;
            }

            printf("DENM ACK timeout (%d / %d received)\n",
                   context->ack_received_count, context->expected_ack_count);
            break;
        }

        if (wait_result != 0)
        {
            fprintf(
                stderr,
                "DENM ACK wait 오류: %d\n",
                wait_result
            );

            break;
        }
    }

    if (context->running &&
        context->ack_received_count >= context->expected_ack_count)
    {
        printf("All DENM ACKs received\n");
        result = 0;
    }

    context->ack_waiting = false;
    pthread_mutex_unlock(&context->ack_mutex);
    return result;
}

/*
 * DENM 자체의 sendto()가 실패한 경우 시작해 둔 ACK 대기 상태를 취소한다.
 */
static void cancel_ack_wait(sender_context_t *context)
{
    pthread_mutex_lock(&context->ack_mutex);
    context->ack_waiting = false;
    pthread_mutex_unlock(&context->ack_mutex);
}

/* ============================================================
 * 6. 종료 자원 정리
 * ============================================================ */

/*
 * 프로그램 종료 시 Sender의 condition variable과 mutex를 해제한다.
 * 모든 worker Thread가 join된 뒤 호출되어야 안전하다.
 */
static void destroy_sync_objects(sender_context_t *context)
{
    pthread_cond_destroy(&context->ack_condition);
    pthread_cond_destroy(&context->heartbeat_condition);
    pthread_mutex_destroy(&context->ack_mutex);
    pthread_mutex_destroy(&context->heartbeat_mutex);
}

/* ============================================================
 * 7. 프로그램 진입점
 * ============================================================ */

/*
 * Sender 프로그램의 전체 실행 흐름.
 *
 * 1) 명령행 인자와 Client IP 목록 검사
 * 2) 동기화 객체 / UDP socket / broadcast 주소 / IPC server 초기화
 * 3) Receive Thread 생성
 * 4) Heartbeat Thread 생성
 * 5) /tmp/p2p.sock에서 사고 이벤트 IPC 수신
 * 6) 이벤트마다 DENM 생성 -> Broadcast -> ACK 대기
 * 7) 종료 요청 후 Thread join / socket close / IPC path 정리
 */
/*
 * ============================================================================
 * main() 읽는 순서
 * ============================================================================
 *
 *  1. 실행 인자 검사
 *  2. sender_context_t 0 초기화
 *  3. expected_ack_count 해석
 *  4. Client IP 목록 초기화
 *  5. mutex / condition variable 초기화
 *  6. UDP socket 생성 및 0.0.0.0:5000 bind
 *  7. DENM Broadcast 목적지 192.168.49.255:5000 준비
 *  8. /tmp/p2p.sock IPC server 생성
 *  9. signal handler 등록
 * 10. Receive Thread 생성
 * 11. Heartbeat Thread 생성
 * 12. Main Thread는 IPC accept/recv loop 진입
 * 13. 사고 이벤트 수신 시 send_denm_event()
 * 14. 종료 요청 시 모든 Thread join
 * 15. IPC / UDP / Unix socket path / pthread 자원 정리
 *
 * main() 자체는 "프로그램 생명주기 관리 + IPC 사고 이벤트 처리"가 핵심이고,
 * UDP 수신과 Heartbeat 감시는 별도 Thread에 맡긴다.
 * ============================================================================
 */
int main(int argc, char *argv[])
{
    
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 3)
    {
        fprintf(
            stderr,
            "Usage: %s <expected_ack_count> <client_ip ...>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    sender_context_t context;
    memset(&context, 0, sizeof(context));

    context.udp_fd = -1;
    context.server_fd = -1;
    context.ipc_fd = -1;
    context.running = 1;

    if (
        parse_positive_int(
            argv[1],
            &context.expected_ack_count
        ) < 0
    )
    {
        fprintf(
            stderr,
            "expected_ack_count는 1~%d 범위여야 합니다.\n",
            MAX_PEERS
        );
        return EXIT_FAILURE;
    }

    context.peer_count = argc - 2;

    if (context.peer_count != context.expected_ack_count)
    {
        fprintf(
            stderr,
            "ACK 예상 개수와 Client IP 개수가 다릅니다.\n"
        );
        return EXIT_FAILURE;
    }

    if (context.peer_count > MAX_PEERS)
    {
        fprintf(stderr, "Client 수가 너무 많습니다.\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < context.peer_count; i++)
    {
        if (
            initialize_peer(
                &context.peers[i],
                argv[i + 2]
            ) < 0
        )
        {
            fprintf(
                stderr,
                "잘못된 Client IP: %s\n",
                argv[i + 2]
            );
            return EXIT_FAILURE;
        }
    }

    if (initialize_sync_objects(&context) < 0)
    {
        fprintf(stderr, "동기화 객체 초기화 실패\n");
        return EXIT_FAILURE;
    }

    context.udp_fd = create_udp_socket();

    if (context.udp_fd < 0)
    {
        destroy_sync_objects(&context);
        return EXIT_FAILURE;
    }

    memset(
        &context.broadcast_address,
        0,
        sizeof(context.broadcast_address)
    );

    context.broadcast_address.sin_family = AF_INET;
    context.broadcast_address.sin_port = htons(NETWORK_PORT);

    if (
        inet_pton(
            AF_INET,
            BROADCAST_IP,
            &context.broadcast_address.sin_addr
        ) != 1
    )
    {
        fprintf(stderr, "잘못된 브로드캐스트 IP입니다.\n");
        close(context.udp_fd);
        destroy_sync_objects(&context);
        return EXIT_FAILURE;
    }

    context.server_fd = ipc_server_create(
        P2P_IPC_SOCKET_PATH
    );

    if (context.server_fd < 0)
    {
        close(context.udp_fd);
        destroy_sync_objects(&context);
        return EXIT_FAILURE;
    }

    static const char *const watchdog_names[] = {"p2p IPC/DENM", "p2p UDP", "p2p peer heartbeat"};
    if (ace_watchdog_start(&watchdog, watchdog_names, 3) != 0)
    {
        perror("watchdog start");
        close(context.server_fd);
        close(context.udp_fd);
        unlink(P2P_IPC_SOCKET_PATH);
        destroy_sync_objects(&context);
        return EXIT_FAILURE;
    }
    global_context = &context;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

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
        ace_watchdog_stop(&watchdog);
        request_stop(&context);
        close(context.server_fd);
        close(context.udp_fd);
        unlink(P2P_IPC_SOCKET_PATH);
        destroy_sync_objects(&context);
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
        ace_watchdog_stop(&watchdog);
        close(context.server_fd);
        close(context.udp_fd);
        unlink(P2P_IPC_SOCKET_PATH);
        destroy_sync_objects(&context);
        return EXIT_FAILURE;
    }

    uint8_t ipc_payload[IPC_PAYLOAD_SIZE];

    while (context.running)
    {
        /*
         * 바깥 while은 "IPC Client 연결 단위"의 반복이다.
         *
         * Sensor/Filter 쪽 IPC Client가 끊겨도 Sender 전체를 종료하지 않고
         * 다시 새로운 IPC Client의 접속을 기다린다.
         */
        if (wait_for_ipc_input(&context, context.server_fd) != 0) break;
        int ipc_fd = ipc_server_accept(context.server_fd);

        if (ipc_fd < 0)
        {
            if (!context.running)
            {
                break;
            }

            continue;
        }

        struct timeval frame_timeout = {2, 0};
        if (setsockopt(ipc_fd, SOL_SOCKET, SO_RCVTIMEO,
                       &frame_timeout, sizeof(frame_timeout)) != 0)
        {
            perror("setsockopt IPC frame timeout");
            ipc_close(ipc_fd);
            break;
        }
        context.ipc_fd = ipc_fd;

        /*
         * 안쪽 while은 "현재 연결된 IPC Client에서 여러 메시지를 계속 받는"
         * 반복이다. 연결이 끊기면 이 while만 빠져나가고 바깥 accept로 돌아간다.
         */
        while (context.running)
        {
            ipc_header_t ipc_header;

            memset(&ipc_header, 0, sizeof(ipc_header));
            memset(ipc_payload, 0, sizeof(ipc_payload));

            if (wait_for_ipc_input(&context, ipc_fd) != 0) break;
            ace_watchdog_progress(&watchdog, WD_IPC, WD_IPC_FRAME_MS);
            int result = ipc_recv(
                ipc_fd,
                &ipc_header,
                ipc_payload,
                sizeof(ipc_payload)
            );

            if (result == IPC_CONNECTION_CLOSED)
            {
                break;
            }

            if (result != IPC_SUCCESS)
            {
                if (context.running)
                {
                    fprintf(
                        stderr,
                        "IPC 수신 오류: %d\n",
                        result
                    );
                }

                break;
            }

            if (ipc_header.type != IPC_MSG_ACCIDENT_EVENT)
            {
                printf(
                    "지원하지 않는 IPC 메시지 type=%u\n",
                    ipc_header.type
                );

                continue;
            }

            if (
                ipc_header.length !=
                sizeof(accident_ipc_event_t)
            )
            {
                fprintf(
                    stderr,
                    "사고 이벤트 Payload 크기 오류: %u\n",
                    ipc_header.length
                );

                continue;
            }

            /* 축소된 Sensor IPC payload를 읽은 뒤 전체 P2P payload로 확장한다. */
            accident_ipc_event_t sensor_event;
            accident_event_payload_t event;

            memcpy(
                &sensor_event,
                ipc_payload,
                sizeof(sensor_event)
            );

            memset(&event, 0, sizeof(event));
            event.station_id = sensor_event.station_id;
            event.sequence_number = sensor_event.sequence_number;
            event.detection_time = sensor_event.detection_time;
            event.reference_time = (int64_t)get_timestamp_its_ms();
            event.latitude = Latitude_unavailable;
            event.longitude = Longitude_unavailable;
            event.speed_value = SpeedValue_unavailable;
            event.heading_value = sensor_event.heading_value;
            event.delta_latitude = DeltaLatitude_unavailable;
            event.delta_longitude = DeltaLongitude_unavailable;
            event.delta_altitude = DeltaAltitude_unavailable;
            event.local_x_mm = sensor_event.local_x_mm;
            event.local_y_mm = sensor_event.local_y_mm;

            ace_watchdog_progress(&watchdog, WD_IPC, WD_DENM_SEND_MS);
            if (
                send_denm_event(
                    &context,
                    &event
                ) < 0
            )
            {
                if (!context.running)
                {
                    break;
                }

                fprintf(stderr, "DENM ACK 확인 실패\n");
            }
        }

        ipc_close(ipc_fd);
        context.ipc_fd = -1;
    }

    request_stop(&context);

    /*
     * request_stop()으로 종료 신호를 보낸 뒤 Thread가 실제로 끝날 때까지 기다린다.
     * Thread가 아직 context / socket / mutex를 사용하는 중일 수 있으므로
     * join보다 먼저 자원을 파괴하면 안 된다.
     */
    pthread_join(context.heartbeat_thread, NULL);
    pthread_join(context.receive_thread, NULL);
    ace_watchdog_stop(&watchdog);

    if (context.ipc_fd >= 0)
    {
        ipc_close(context.ipc_fd);
        context.ipc_fd = -1;
    }

    if (context.server_fd >= 0)
    {
        ipc_close(context.server_fd);
        context.server_fd = -1;
    }

    if (context.udp_fd >= 0)
    {
        close(context.udp_fd);
        context.udp_fd = -1;
    }

    unlink(P2P_IPC_SOCKET_PATH);

    destroy_sync_objects(&context);
    global_context = NULL;

    return EXIT_SUCCESS;
}
