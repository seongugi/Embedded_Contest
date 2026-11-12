#define _GNU_SOURCE
#include "ipc.h"
#include "protocol.h"
#include "sensor_protocol.h"
#include "filter_protocol.h"
#include "inference_protocol.h"
#include "runtime_watchdog.h"
#include "witness_fov.h"
#include "witness_radius_config.h"
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <gpiod.h>
#include <inttypes.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Sensor 상태를 전달받을 Unix Domain Socket.
 *
 * Sensor 프로세스는 차량의 현재 위치와 heading을
 * 계속 이 소켓으로 전송한다.
 */
#define SENSOR_SOCKET_PATH "/tmp/witness_sensor.sock"

/*
 * V2V 사고 이벤트를 전달받을 Unix Domain Socket.
 *
 * V2V/P2P 처리 프로세스가 DENM에서 필요한 정보를
 * 추출한 뒤 AccidentEvent 형태로 이 소켓에 전달한다.
 */
#define V2V_SOCKET_PATH "/tmp/filter.sock"
enum { FILTER_WD_SENSOR, FILTER_WD_V2V, FILTER_WD_WORKER };
static ace_watchdog_t filter_watchdog;

/* Idle sockets renew progress every second. Once a frame starts, its entire
 * header and payload must arrive within two seconds; only that client closes. */
static int filter_wait_input(int fd, unsigned slot)
{
    struct pollfd input = {fd, POLLIN, 0};
    for (;;) {
        int result;
        ace_watchdog_progress(&filter_watchdog, slot, 10000U);
        result = poll(&input, 1, 1000);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (input.revents & POLLNVAL)) return -1;
        if (result > 0) return 0;
    }
}

static int filter_accept(int fd, unsigned slot)
{
    for (;;) {
        int client;
        if (filter_wait_input(fd, slot) != 0) return -1;
        client = accept(fd, NULL, NULL);
        if (client >= 0) return client;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
}

static int filter_read_until(int fd, void *buffer, size_t length, uint64_t deadline)
{
    size_t offset = 0;
    while (offset < length) {
        struct pollfd input = {fd, POLLIN, 0};
        uint64_t now = ace_watchdog_now_ns();
        int wait_ms, result;
        ssize_t count;
        if (now >= deadline) return IPC_ERROR_RECV;
        wait_ms = (int)((deadline - now + 999999U) / 1000000U);
        result = poll(&input, 1, wait_ms);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (input.revents & POLLNVAL)) return IPC_ERROR_RECV;
        count = recv(fd, (char *)buffer + offset, length - offset, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) return IPC_ERROR_RECV;
        if (count == 0) return IPC_CONNECTION_CLOSED;
        offset += (size_t)count;
    }
    return IPC_SUCCESS;
}

static int filter_receive(int fd, ipc_header_t *header, void *payload,
                           size_t capacity, unsigned slot)
{
    uint64_t deadline;
    int result;
    if (filter_wait_input(fd, slot) != 0) return IPC_ERROR_RECV;
    deadline = ace_watchdog_now_ns() + UINT64_C(2000000000);
    result = filter_read_until(fd, header, sizeof(*header), deadline);
    if (result != IPC_SUCCESS) return result;
    if (header->length > capacity) return IPC_ERROR_INVALID_LENGTH;
    return filter_read_until(fd, payload, header->length, deadline);
}

static void filter_wait_condition(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec++;
    ace_watchdog_progress(&filter_watchdog, FILTER_WD_WORKER, 10000U);
    (void)pthread_cond_clockwait(condition, mutex, CLOCK_MONOTONIC, &deadline);
}

#define ACTION_ID_TEXT_SIZE 32U
#define LED_GPIO_OFFSET 4U
#define INFERENCE_SEND_TIMEOUT_US 200000
#define INFERENCE_RETRY_INTERVAL_NS 500000000L


/*
 * 현재 사용하는 카메라의 수평 화각.
 *
 * 실제 차량/모형차에 카메라를 장착한 뒤
 * 영상에서 실제 유효 화각을 확인하여 수정한다.
 *
 * 현재 설계에서는:
 *
 *     차량 heading == 카메라 중심 방향
 *
 * 으로 간주한다.
 * 
 * 관측차는 고정 배치하므로 최초 정상 위치를 확보한 뒤에는 유지한다.
 */
#define CAMERA_FOV_DEG WITNESS_CAMERA_FOV_DEG

/* Initialized once before any worker starts; distance and radius use mm. */
static double target_body_radius_mm;

/*
 * ==================================================
 * Accident Event Queue Node
 * ==================================================
 *
 * Queue에 저장되는 하나의 사고 이벤트다.
 *
 * next가 다음 Node를 가리키므로
 * 여러 Node가 연결된 단일 연결 리스트 형태가 된다.
 *
 * 예:
 *
 *     [Event A] -> [Event B] -> [Event C] -> NULL
 */
typedef struct accident_node
{
    accident_event_t event;

    struct accident_node *next;

} accident_node_t;


/*
 * ==================================================
 * Accident Event Queue
 * ==================================================
 *
 * 사고 이벤트를 FIFO 방식으로 저장한다.
 *
 * head:
 *     가장 먼저 들어온 이벤트
 *
 * tail:
 *     가장 최근에 들어온 이벤트
 *
 * mutex:
 *     V2V Thread와 Filter Worker가
 *     동시에 Queue를 접근하는 것을 방지한다.
 *
 * cond:
 *     Worker가 Queue가 비어 있을 때 잠들었다가
 *     새로운 사고 이벤트가 들어오면 깨어나도록 한다.
 */
typedef struct
{
    accident_node_t *head;
    accident_node_t *tail;

    pthread_mutex_t mutex;
    pthread_cond_t cond;

} accident_queue_t;


/* Witness Candidate LED를 전용 thread에서 monotonic deadline으로 관리한다. */
struct led_controller
{
    struct gpiod_chip *chip;
    struct gpiod_line_request *request;
    unsigned int offset;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct timespec deadline;
    int thread_started;
    int active;
    int closed;
};


/*
 * ==================================================
 * Latest Vehicle State
 * ==================================================
 *
 * Sensor Thread가 정상 위치를 저장하고 최초 수신을 기다리는 Worker를 깨운다.
 *
 * Filter Worker가 사고 이벤트를 처리할 때
 * 이 값을 복사해서 사용한다.
 *
 * 다른 Thread가 동시에 읽고 쓸 수 있기 때문에
 * vehicle_state_mutex로 보호한다.
 */
static vehicle_state_t latest_vehicle_state;
static int vehicle_state_valid = 0;
static pthread_mutex_t vehicle_state_mutex =
    PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vehicle_state_ready = PTHREAD_COND_INITIALIZER;


/*
 * 사고 이벤트 Queue.
 *
 * 이 파일 내부에서만 사용하는 전역 상태이므로
 * static으로 제한한다.
 */
static accident_queue_t accident_queue =
{
    .head = NULL,
    .tail = NULL,

    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};


static void *led_controller_thread(void *argument)
{
    struct led_controller *led = (struct led_controller *)argument;

    pthread_mutex_lock(&led->mutex);
    while (!led->closed)
    {
        int wait_result;

        while (!led->active && !led->closed)
            pthread_cond_wait(&led->condition, &led->mutex);

        if (led->closed)
            break;

        wait_result = pthread_cond_timedwait(
            &led->condition,
            &led->mutex,
            &led->deadline
        );
        if (wait_result == 0)
            continue;

        if (wait_result != ETIMEDOUT)
            fprintf(stderr,
                    "LED condition timed wait 실패: %s\n",
                    strerror(wait_result));

        /* Timeout 뒤 잠금을 다시 얻기 전에 연장된 점등 기한을 재확인한다. */
        if (wait_result == ETIMEDOUT)
        {
            struct timespec now;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                perror("LED CLOCK_MONOTONIC");
            else if (now.tv_sec < led->deadline.tv_sec ||
                     (now.tv_sec == led->deadline.tv_sec &&
                      now.tv_nsec < led->deadline.tv_nsec))
                continue;
        }

        if (gpiod_line_request_set_value(
                led->request,
                led->offset,
                GPIOD_LINE_VALUE_INACTIVE) != 0)
            perror("GPIO4 LED OFF");
        led->active = 0;
    }

    if (gpiod_line_request_set_value(
            led->request,
            led->offset,
            GPIOD_LINE_VALUE_INACTIVE) != 0)
        perror("GPIO4 종료 LED OFF");
    led->active = 0;
    pthread_mutex_unlock(&led->mutex);
    return NULL;
}


static int led_controller_init(struct led_controller *led)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    pthread_condattr_t condition_attribute;
    unsigned int offset = LED_GPIO_OFFSET;
    int mutex_initialized = 0;
    int condition_attribute_initialized = 0;
    int condition_initialized = 0;
    int result = -1;

    memset(led, 0, sizeof(*led));
    led->offset = offset;

    if (pthread_mutex_init(&led->mutex, NULL) != 0)
        goto cleanup;
    mutex_initialized = 1;

    if (pthread_condattr_init(&condition_attribute) != 0)
        goto cleanup;
    condition_attribute_initialized = 1;

    if (pthread_condattr_setclock(&condition_attribute, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&led->condition, &condition_attribute) != 0)
        goto cleanup;
    condition_initialized = 1;

    led->chip = gpiod_chip_open("/dev/gpiochip0");
    if (led->chip == NULL)
    {
        perror("GPIO chip open");
        goto cleanup;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if (settings == NULL || line_config == NULL || request_config == NULL)
        goto gpio_failure;

    if (gpiod_line_settings_set_direction(
            settings, GPIOD_LINE_DIRECTION_OUTPUT) != 0 ||
        gpiod_line_settings_set_output_value(
            settings, GPIOD_LINE_VALUE_INACTIVE) != 0 ||
        gpiod_line_config_add_line_settings(
            line_config, &offset, 1, settings) != 0)
        goto gpio_failure;

    gpiod_request_config_set_consumer(request_config, "witness-filter-led");
    led->request = gpiod_chip_request_lines(
        led->chip,
        request_config,
        line_config
    );
    if (led->request == NULL)
        goto gpio_failure;

    if (pthread_create(&led->thread, NULL,
                       led_controller_thread, led) != 0)
    {
        fprintf(stderr, "LED thread 생성 실패\n");
        goto cleanup;
    }
    led->thread_started = 1;
    result = 0;
    goto cleanup;

gpio_failure:
    perror("GPIO4 LED 초기화");

cleanup:
    gpiod_request_config_free(request_config);
    gpiod_line_config_free(line_config);
    gpiod_line_settings_free(settings);

    if (condition_attribute_initialized)
        pthread_condattr_destroy(&condition_attribute);

    if (result != 0)
    {
        if (led->request != NULL)
            gpiod_line_request_release(led->request);
        if (led->chip != NULL)
            gpiod_chip_close(led->chip);
        if (condition_initialized)
            pthread_cond_destroy(&led->condition);
        if (mutex_initialized)
            pthread_mutex_destroy(&led->mutex);
        memset(led, 0, sizeof(*led));
        fprintf(stderr, "LED controller 시작 실패\n");
    }

    return result;
}


/* Candidate 발생 즉시 켜고 마지막 발생 시점부터 5초로 deadline을 갱신한다. */
static int led_controller_trigger(struct led_controller *led)
{
    int result = 0;

    pthread_mutex_lock(&led->mutex);
    if (led->closed || clock_gettime(CLOCK_MONOTONIC, &led->deadline) != 0)
    {
        result = -1;
    }
    else
    {
        led->deadline.tv_sec += 5;
        if (gpiod_line_request_set_value(
                led->request,
                led->offset,
                GPIOD_LINE_VALUE_ACTIVE) != 0)
        {
            perror("GPIO4 LED ON");
            result = -1;
        }
        else
        {
            led->active = 1;
            pthread_cond_signal(&led->condition);
        }
    }
    pthread_mutex_unlock(&led->mutex);

    return result;
}


static void led_controller_destroy(struct led_controller *led)
{
    pthread_mutex_lock(&led->mutex);
    led->closed = 1;
    pthread_cond_signal(&led->condition);
    pthread_mutex_unlock(&led->mutex);

    if (led->thread_started)
        pthread_join(led->thread, NULL);

    if (led->request != NULL)
    {
        if (gpiod_line_request_set_value(
                led->request,
                led->offset,
                GPIOD_LINE_VALUE_INACTIVE) != 0)
            perror("GPIO4 release 전 LED OFF");
        gpiod_line_request_release(led->request);
    }
    if (led->chip != NULL)
        gpiod_chip_close(led->chip);

    pthread_cond_destroy(&led->condition);
    pthread_mutex_destroy(&led->mutex);
    memset(led, 0, sizeof(*led));
}


/*
 * ==================================================
 * Accident Queue Push
 * ==================================================
 *
 * V2V Thread가 받은 사고 이벤트를 Queue 뒤에 넣는다.
 *
 * V2V Thread는 사고 이벤트를 직접 계산하지 않는다.
 *
 *     V2V IPC 수신
 *          ↓
 *     Queue Push
 *          ↓
 *     다음 IPC 수신
 *
 * 따라서 Filter 계산 때문에 V2V 수신이 막히지 않는다.
 */
static int accident_queue_push(
    const accident_event_t *event
)
{
    /*
     * 새로운 Queue Node를 동적으로 생성한다.
     */
    accident_node_t *node =
        malloc(sizeof(accident_node_t));

    if (node == NULL)
    {
        perror("malloc");
        return -1;
    }

    /*
     * 사고 이벤트를 Node에 복사한다.
     */
    node->event = *event;

    /*
     * 새 Node는 현재 Queue의 마지막 Node가 되므로
     * 다음 Node는 아직 존재하지 않는다.
     */
    node->next = NULL;


    /*
     * Queue 구조를 변경하므로 mutex를 획득한다.
     */
    pthread_mutex_lock(
        &accident_queue.mutex
    );


    /*
     * Queue가 비어 있다면
     * 새 Node가 동시에 head와 tail이 된다.
     */
    if (accident_queue.tail == NULL)
    {
        accident_queue.head = node;
        accident_queue.tail = node;
    }
    else
    {
        /*
         * 기존 tail 뒤에 새 Node를 연결한다.
         */
        accident_queue.tail->next = node;

        /*
         * 새 Node를 새로운 tail로 만든다.
         */
        accident_queue.tail = node;
    }


    /*
     * Queue가 비어 있어서 잠들어 있을 수 있는
     * Filter Worker에게 새로운 이벤트가 들어왔음을 알린다.
     *
     * Worker는 condition variable에서 깨어나
     * Queue를 다시 확인한다.
     */
    pthread_cond_signal(
        &accident_queue.cond
    );


    /*
     * Queue 조작이 끝났으므로 mutex를 해제한다.
     */
    pthread_mutex_unlock(
        &accident_queue.mutex
    );

    return 0;
}


/*
 * ==================================================
 * Accident Queue Pop
 * ==================================================
 *
 * Filter Worker가 Queue 앞에서 사고 이벤트를 꺼낸다.
 *
 * FIFO:
 *
 *     A -> B -> C
 *
 *     Pop
 *      ↓
 *     A 제거
 *
 *     B -> C
 */
static int accident_queue_pop(
    accident_event_t *event
)
{
    /*
     * Queue를 읽고 변경하기 전에 mutex를 획득한다.
     */
    pthread_mutex_lock(
        &accident_queue.mutex
    );


    /*
     * Queue가 비어 있으면
     * 새로운 사고 이벤트가 들어올 때까지 기다린다.
     *
     * pthread_cond_wait()는:
     *
     *     1. mutex를 잠시 해제하고
     *     2. Thread를 sleep 상태로 만들고
     *     3. signal이 오면 다시 깨어나고
     *     4. mutex를 다시 획득한 뒤
     *        아래 조건을 다시 확인한다.
     *
     * 따라서 사고가 없을 때 Worker가
     * 무한히 CPU를 사용하면서 Queue를 검사하지 않는다.
     */
    while (accident_queue.head == NULL)
    {
        filter_wait_condition(&accident_queue.cond, &accident_queue.mutex);
    }


    /*
     * Queue의 가장 앞쪽 Node를 가져온다.
     */
    accident_node_t *node =
        accident_queue.head;


    /*
     * 다음 Node를 새로운 head로 설정한다.
     */
    accident_queue.head =
        node->next;


    /*
     * Pop 결과 Queue가 비었다면
     * tail도 NULL로 만든다.
     */
    if (accident_queue.head == NULL)
    {
        accident_queue.tail = NULL;
    }


    /*
     * Queue 구조 변경이 끝났으므로 mutex를 해제한다.
     */
    pthread_mutex_unlock(
        &accident_queue.mutex
    );


    /*
     * Node에 저장된 사고 이벤트를
     * 호출자에게 복사한다.
     */
    *event = node->event;


    /*
     * Queue Node는 동적 할당했으므로
     * 사용이 끝난 뒤 메모리를 반환한다.
     */
    free(node);

    return 0;
}


/*
 * ==================================================
 * Vehicle State Snapshot
 * ==================================================
 *
 * Sensor Thread가 계속 갱신하는 최신 차량 상태를
 * Filter Worker가 안전하게 복사한다.
 *
 * 중요한 점:
 *
 *     mutex lock
 *          ↓
 *     구조체 복사
 *          ↓
 *     mutex unlock
 *          ↓
 *     실제 계산
 *
 * 계산 자체는 mutex 안에서 하지 않는다.
 */

static int get_vehicle_state_snapshot(
    vehicle_state_t *state
)
{
    pthread_mutex_lock(
        &vehicle_state_mutex
    );

    /* 최초 위치보다 먼저 받은 사고는 Worker가 보관한 채 기다린다. */
    while (!vehicle_state_valid)
    {
        filter_wait_condition(&vehicle_state_ready, &vehicle_state_mutex);
    }

    /* 고정 배치의 정상값은 IPC 단절/센서 재시작 동안에도 유지한다. */
    *state = latest_vehicle_state;

    pthread_mutex_unlock(
        &vehicle_state_mutex
    );

    return 0;
}

static int publish_vehicle_state(const vehicle_state_t *state)
{
    if (!isfinite(state->x_mm) || !isfinite(state->y_mm) ||
        !isfinite(state->heading_deg) ||
        fabsf(state->x_mm) > 1000000.0f ||
        fabsf(state->y_mm) > 1000000.0f ||
        state->heading_deg < 0.0f || state->heading_deg >= 360.0f)
        return -1;

    pthread_mutex_lock(&vehicle_state_mutex);
    latest_vehicle_state = *state;
    vehicle_state_valid = 1;
    pthread_cond_broadcast(&vehicle_state_ready);
    pthread_mutex_unlock(&vehicle_state_mutex);
    return 0;
}

/* The physical camera FOV remains 64 degrees (32 degrees each side).
 * witness_evaluate_body_fov() includes the target body's angular extent when
 * selecting first-stage candidates; image confirmation still belongs to AI.
 */
static int send_event_to_inference(const accident_event_t *event);


/*
 * ==================================================
 * Accident Filter
 * ==================================================
 *
 * Filter Worker가 Queue에서 받은 사고 이벤트와
 * 현재 차량 상태를 비교한다.
 *
 * 현재 1차 필터에서 사용하는 정보:
 *
 *     사고 차량 X/Y
 *     사고 차량 Heading
 *     내 차량 X/Y
 *     내 차량 Heading
 *
 * 실제 FOV 판정에는
 * 사고 차량의 Heading은 사용하지 않는다.
 *
 * 사고 위치가 내 카메라의 시야 안에 있는지를
 * 판단하기 때문이다.
 */
static void process_accident_event(
    const accident_event_t *event,
    struct led_controller *led
)
{
    vehicle_state_t vehicle_state;
    char action_id[ACTION_ID_TEXT_SIZE];

    snprintf(
        action_id,
        sizeof(action_id),
        "%" PRIu64,
        ((uint64_t)event->station_id << 16) |
            (uint64_t)event->sequence_number);
	
    /*
     * 사고 이벤트 처리 시점의
     * 최신 차량 상태를 복사한다.
     */
    if (
        get_vehicle_state_snapshot(
            &vehicle_state
        ) != 0
    )
    {
        printf(
            "[FILTER] Vehicle state unavailable. "
            "Action ID=%s\n",
            action_id
        );

        return;
    }


    witness_fov_result_t geometry;
    if (witness_evaluate_body_fov(
            vehicle_state.x_mm, vehicle_state.y_mm, vehicle_state.heading_deg,
            event->x_mm, event->y_mm, target_body_radius_mm, &geometry) != 0) {
        fprintf(stderr, "[FILTER] Invalid geometry; Action ID=%s\n", action_id);
        return;
    }
    int witness_candidate = geometry.candidate;


    /* 판정 결과를 출력하고 Candidate이면 LED를 켠 뒤 Inference Event를 보낸다. */
    printf("\n");
    printf("========================================\n");
    printf(" Witness Filter Result\n");
    printf("========================================\n");

    printf(
    "Action ID         : %s\n",
    action_id
);

    printf(
        "Accident XY       : %.1f, %.1f mm\n",
        event->x_mm,
        event->y_mm
    );

    printf(
        "Vehicle XY        : %.1f, %.1f mm\n",
        vehicle_state.x_mm,
        vehicle_state.y_mm
    );

    printf(
        "Vehicle Heading   : %.2f deg\n",
        vehicle_state.heading_deg
    );

    printf(
        "Distance          : %.2f mm\n",
        geometry.distance_mm
    );

    printf(
        "Accident Bearing  : %.2f deg\n",
        geometry.bearing_deg
    );

    printf(
        "Heading Error     : %.2f deg\n",
        geometry.heading_error_deg
    );

    printf(
        "Camera FOV        : %.2f deg (±%.2f deg)\n",
        CAMERA_FOV_DEG,
        CAMERA_FOV_DEG / 2.0f
    );

    printf("Target Radius     : %.3f mm\n", target_body_radius_mm);
    printf("Body Half Angle   : %.3f deg\n", geometry.body_half_angle_deg);
    printf("Candidate Limit   : %.3f deg%s\n",
           geometry.candidate_half_angle_deg,
           geometry.observer_inside_body ? " (observer inside target radius)" : "");

    printf(
        "Witness Candidate : %s\n",
        witness_candidate ? "YES" : "NO"
    );

    if (witness_candidate) {
        if (led_controller_trigger(led) != 0)
            fprintf(stderr, "[FILTER] Witness Candidate LED trigger 실패\n");

        int retry_logged = 0;
        while (send_event_to_inference(event) != 0) {
            ace_watchdog_progress(&filter_watchdog, FILTER_WD_WORKER, 10000U);
            struct timespec retry_delay = { 0, INFERENCE_RETRY_INTERVAL_NS };
            if (!retry_logged) {
                fprintf(stderr,
                        "[FILTER] Inference unavailable; retaining "
                        "Action ID=%s for retry\n", action_id);
                retry_logged = 1;
            }
            while (nanosleep(&retry_delay, &retry_delay) != 0 && errno == EINTR)
                ;
        }
    }

    printf("========================================\n");
}


/*
 * ==================================================
 * Sensor Thread
 * ==================================================
 *
 * Sensor 프로세스와 IPC 연결을 유지하면서
 * 최신 VehicleState를 계속 수신한다.
 *
 * 이 Thread는 센서 데이터 수신만 담당한다.
 *
 *     Sensor IPC
 *          ↓
 *     VehicleState
 *          ↓
 *     latest_vehicle_state
 *
 * 사고 판정은 수행하지 않는다.
 */
static void *sensor_thread_main(
    void *argument
)
{
    int server_fd = *(const int *)argument;

    printf(
        "[SENSOR] Listening: %s\n",
        SENSOR_SOCKET_PATH
    );


    /*
     * Sensor 프로세스 연결을 계속 기다린다.
     */
    while (1)
    {
        int client_fd =
            filter_accept(server_fd, FILTER_WD_SENSOR);

        if (client_fd < 0)
        {
            perror("Filter listening socket");
            return NULL;
        }


        printf(
            "[SENSOR] Client connected\n"
        );


        /*
         * 연결된 Sensor 프로세스에서
         * VehicleState를 계속 받는다.
         */
        while (1)
        {
            ipc_header_t header;
            vehicle_state_t state;


            int result =
                filter_receive(client_fd, &header, &state,
                               sizeof(state), FILTER_WD_SENSOR);


            /*
             * Sensor 프로세스가 연결을 끊으면
             * 현재 연결만 닫고 다시 accept()한다.
             */
            if (
                result ==
                IPC_CONNECTION_CLOSED
            )
            {
                printf(
                    "[SENSOR] Client disconnected\n"
                );

                break;
            }


            /*
             * IPC 자체에서 오류가 발생하면
             * 현재 연결을 종료하고 다시 연결을 기다린다.
             */
            if (result != IPC_SUCCESS)
            {
                fprintf(
                    stderr,
                    "[SENSOR] IPC receive error: %d\n",
                    result
                );

                break;
            }


            /*
             * Sensor 데이터가 아닌 메시지는 무시한다.
             */
            if (
                header.type !=
                IPC_MSG_SENSOR_DATA
            )
            {
                fprintf(
                    stderr,
                    "[SENSOR] Unexpected message type: %u\n",
                    header.type
                );

                continue;
            }


            /*
             * Payload 크기가 VehicleState 구조체와
             * 정확히 일치하는지 확인한다.
             */
            if (
                header.length !=
                sizeof(vehicle_state_t)
            )
            {
                fprintf(
                    stderr,
                    "[SENSOR] Invalid payload size: %u\n",
                    header.length
                );

                continue;
            }


            /*
             * 최신 차량 상태를 갱신한다.
             *
             * Filter Worker가 동시에 읽을 수 있으므로
             * mutex로 보호한다.
             */
            if (publish_vehicle_state(&state) != 0)
                fprintf(stderr, "[SENSOR] Invalid position/heading ignored\n");
        }


        /*
         * 현재 Sensor 연결이 종료되었으므로
         * Client FD를 닫는다.
         */
        ipc_close(client_fd);
    }


    /*
     * 현재 구조에서는 무한 루프이므로
     * 일반적으로 도달하지 않는다.
     */
    ipc_close(server_fd);

    return NULL;
}


/*
 * ==================================================
 * V2V Thread
 * ==================================================
 *
 * V2V/P2P 프로세스로부터 사고 이벤트를 수신한다.
 *
 * 이 Thread에서 중요한 원칙:
 *
 *     사고 이벤트 수신
 *          ↓
 *     Queue Push
 *          ↓
 *     즉시 다음 IPC 수신
 *
 * 위치 계산이나 FOV 계산은 여기서 하지 않는다.
 *
 * 따라서 Filter 계산이 오래 걸려도
 * V2V IPC 수신이 직접 막히지 않는다.
 */
static void *v2v_thread_main(
    void *argument
)
{
    int server_fd = *(const int *)argument;

    printf(
        "[V2V] Listening: %s\n",
        V2V_SOCKET_PATH
    );


    /*
     * V2V 연결을 계속 기다린다.
     */
    while (1)
    {
        int client_fd =
            filter_accept(server_fd, FILTER_WD_V2V);

        if (client_fd < 0)
        {
            perror("Filter listening socket");
            return NULL;
        }


        printf(
            "[V2V] Client connected\n"
        );


        /*
         * 연결된 V2V 프로세스에서
         * 사고 이벤트를 계속 수신한다.
         */
        while (1)
        {
            ipc_header_t header;
            accident_event_t event;


            int result =
                filter_receive(client_fd, &header, &event,
                               sizeof(event), FILTER_WD_V2V);


            /*
             * V2V 프로세스가 연결을 종료하면
             * 현재 연결만 닫고 다시 accept()한다.
             */
            if (
                result ==
                IPC_CONNECTION_CLOSED
            )
            {
                printf(
                    "[V2V] Client disconnected\n"
                );

                break;
            }


            /*
             * IPC 오류가 발생하면
             * 현재 연결을 종료한다.
             */
            if (result != IPC_SUCCESS)
            {
                fprintf(
                    stderr,
                    "[V2V] IPC receive error: %d\n",
                    result
                );

                break;
            }


            /*
             * Filter가 기대하는
             * 사고 확인 메시지인지 검사한다.
             */
            if (
                header.type !=
                IPC_MSG_ACCIDENT_CONFIRM
            )
            {
                fprintf(
                    stderr,
                    "[V2V] Unexpected message type: %u\n",
                    header.type
                );

                continue;
            }


            /*
             * 사고 이벤트 구조체 크기와
             * 실제 IPC Payload 크기가 일치하는지 확인한다.
             */
            if (
                header.length !=
                sizeof(accident_event_t)
            )
            {
                fprintf(
                    stderr,
                    "[V2V] Invalid payload size: %u\n",
                    header.length
                );

                continue;
            }

            char action_id[ACTION_ID_TEXT_SIZE];

            snprintf(
                action_id,
                sizeof(action_id),
                "%" PRIu64,
                ((uint64_t)event.station_id << 16) |
                    (uint64_t)event.sequence_number);
           
printf(
    "[V2V] Accident Event received "
    "(Action ID=%s)\n",
    action_id
            );


            /*
             * 사고 이벤트를 Filter Worker에게 넘긴다.
             *
             * 여기서는 계산하지 않는다.
             */
            if (
                accident_queue_push(
                    &event
                ) != 0
            )
            {
                fprintf(
                    stderr,
                    "[V2V] Failed to queue accident event\n"
                );
            }
        }


        /*
         * 현재 V2V 연결이 종료되었으므로
         * Client FD를 닫는다.
         */
        ipc_close(client_fd);
    }


    /*
     * 현재 구조에서는 무한 루프이므로
     * 일반적으로 도달하지 않는다.
     */
    ipc_close(server_fd);

    return NULL;
}


/*
 * ==================================================
 * Filter Worker Thread
 * ==================================================
 *
 * Queue에서 사고 이벤트를 하나씩 꺼내서
 * 실제 Witness 후보 판정을 수행한다.
 *
 *     Queue
 *       ↓
 *     Accident Event
 *       ↓
 *     최신 VehicleState Snapshot
 *       ↓
 *     거리 계산
 *       ↓
 *     Bearing 계산
 *       ↓
 *     Heading Error
 *       ↓
 *     FOV 판정
 */
static void *filter_worker_main(
    void *argument
)
{
    struct led_controller *led = (struct led_controller *)argument;


    printf(
        "[FILTER] Worker started\n"
    );


    /*
     * 사고 이벤트를 계속 처리한다.
     *
     * Queue가 비어 있으면
     * accident_queue_pop() 내부에서 sleep한다.
     */
    while (1)
    {
        accident_event_t event;
        ace_watchdog_progress(&filter_watchdog, FILTER_WD_WORKER, 10000U);

        /*
         * Queue에서 사고 이벤트 하나를 기다린다.
         */
        if (
            accident_queue_pop(
                &event
            ) != 0
        )
        {
            continue;
        }


        /*
         * 실제 위치/FOV 판정은
         * Filter Worker가 담당한다.
         */
        process_accident_event(
            &event,
            led
        );
    }


    return NULL;
}

/*
 * ==================================================
 * Inference Event 전송 IPC
 * ==================================================
 *
 * Witness Candidate가 맞으면 위치 정보는 제외하고 station/sequence/사고 시각만
 * 공용 IPC를 통해 AI Inference 프로세스에 전달한다.
 */
static int send_event_to_inference(
    const accident_event_t *event)
{
    inference_event_t inference_event;
    ipc_header_t header;
    struct timespec now;
    int socket_fd;
    struct sockaddr_un address;
    const struct timeval timeout = { 0, INFERENCE_SEND_TIMEOUT_US };

    if (event == NULL)
        return -1;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        return -1;
    }

    memset(&inference_event, 0, sizeof(inference_event));
    memset(&header, 0, sizeof(header));
    inference_event.station_id = event->station_id;
    inference_event.sequence_number = event->sequence_number;
    inference_event.detection_time = event->detection_time;

    /* connect 전에 제한 시간을 설정해 AF_UNIX backlog 대기도 제한한다. */
    socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
             INFERENCE_SOCKET_PATH);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) != 0 ||
        connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ipc_close(socket_fd);
        return -1;
    }

    header.type = IPC_MSG_INFERENCE_EVENT;
    header.flags = IPC_FLAG_NONE;
    header.sequence = event->sequence_number;
    header.timestamp =
        (uint64_t)now.tv_sec * 1000000000ULL +
        (uint64_t)now.tv_nsec;

    if (ipc_send(
            socket_fd,
            &header,
            &inference_event,
            sizeof(inference_event)) != IPC_SUCCESS) {
        fprintf(stderr,
                "[FILTER] failed to send inference event\n");
        ipc_close(socket_fd);
        return -1;
    }

    ipc_close(socket_fd);
    printf(
        "[FILTER] inference event sent: station=%" PRIu32
        " sequence=%" PRIu16 " detection_ns=%" PRId64 "\n",
        inference_event.station_id,
        inference_event.sequence_number,
        inference_event.detection_time);

    return 0;
}
/*
 * ==================================================
 * Main
 * ==================================================
 *
 * Witness Filter 프로세스의 Thread들을 생성한다.
 *
 *     Main
 *       ├── LED Timer Thread
 *       ├── Sensor Thread
 *       ├── V2V Thread
 *       └── Filter Worker
 *
 * Main 자체는 네 Thread를 생성한 뒤
 * pthread_join()으로 각각의 실행을 유지한다.
 *
 * 별도의 signal handler나 graceful shutdown은
 * 현재 구조에서는 사용하지 않는다.
 *
 * Ctrl+C 등으로 프로세스가 종료되면
 * OS가 프로세스 전체를 종료시킨다.
 */
int main(void)
{
    struct led_controller led;
    pthread_t sensor_thread;
    pthread_t v2v_thread;
    pthread_t filter_worker;

    int result;
    int sensor_server_fd;
    int v2v_server_fd;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (witness_parse_target_radius_mm(getenv(WITNESS_TARGET_RADIUS_ENV),
                                      WITNESS_DEFAULT_TARGET_RADIUS_MM,
                                      &target_body_radius_mm) != 0) {
        fprintf(stderr, "[FILTER] %s must be a finite, non-negative radius in mm\n",
                WITNESS_TARGET_RADIUS_ENV);
        return EXIT_FAILURE;
    }

    /* 상대 종료에 따른 송신 실패는 IPC 오류로 받아 재시도한다. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return EXIT_FAILURE;

    /* 두 필수 수신 소켓을 준비한 뒤 수신 스레드를 시작한다. */
    sensor_server_fd = ipc_server_create(SENSOR_SOCKET_PATH);
    if (sensor_server_fd < 0) {
        fprintf(stderr, "[SENSOR] Failed to create server\n");
        return EXIT_FAILURE;
    }
    v2v_server_fd = ipc_server_create(V2V_SOCKET_PATH);
    if (v2v_server_fd < 0) {
        fprintf(stderr, "[V2V] Failed to create server\n");
        ipc_close(sensor_server_fd);
        unlink(SENSOR_SOCKET_PATH);
        return EXIT_FAILURE;
    }

    if (fcntl(sensor_server_fd, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(v2v_server_fd, F_SETFL, O_NONBLOCK) != 0) {
        perror("Filter nonblocking listeners");
        ipc_close(sensor_server_fd);
        ipc_close(v2v_server_fd);
        unlink(SENSOR_SOCKET_PATH);
        unlink(V2V_SOCKET_PATH);
        return EXIT_FAILURE;
    }

    /* 기존 Filter thread보다 먼저 GPIO4를 OFF 상태로 요청한다. */
    if (led_controller_init(&led) != 0) {
        ipc_close(sensor_server_fd);
        ipc_close(v2v_server_fd);
        unlink(SENSOR_SOCKET_PATH);
        unlink(V2V_SOCKET_PATH);
        return EXIT_FAILURE;
    }


    {
        static const char *const names[] = {"sensor IPC", "P2P IPC", "FOV worker"};
        if (ace_watchdog_start(&filter_watchdog, names, 3U) != 0) {
            perror("Filter watchdog start");
            led_controller_destroy(&led);
            return EXIT_FAILURE;
        }
    }

    /*
     * --------------------------------------------------
     * Sensor Thread 생성
     * --------------------------------------------------
     */
    result =
        pthread_create(
            &sensor_thread,
            NULL,
            sensor_thread_main,
            &sensor_server_fd
        );

    if (result != 0)
    {
        fprintf(
            stderr,
            "Failed to create Sensor Thread: %s\n",
            strerror(result)
        );

        led_controller_destroy(&led);
        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * V2V Thread 생성
     * --------------------------------------------------
     */
    result =
        pthread_create(
            &v2v_thread,
            NULL,
            v2v_thread_main,
            &v2v_server_fd
        );

    if (result != 0)
    {
        fprintf(
            stderr,
            "Failed to create V2V Thread: %s\n",
            strerror(result)
        );

        led_controller_destroy(&led);
        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * Filter Worker 생성
     * --------------------------------------------------
     */
    result =
        pthread_create(
            &filter_worker,
            NULL,
            filter_worker_main,
            &led
        );

    if (result != 0)
    {
        fprintf(
            stderr,
            "Failed to create Filter Worker: %s\n",
            strerror(result)
        );

        led_controller_destroy(&led);
        return EXIT_FAILURE;
    }


    /*
     * 프로그램 시작 상태 출력.
     */
    printf("\n");
    printf("========================================\n");
    printf(" Witness Filter Started\n");
    printf("========================================\n");

    printf(
        "Sensor IPC : %s\n",
        SENSOR_SOCKET_PATH
    );

    printf(
        "V2V IPC    : %s\n",
        V2V_SOCKET_PATH
    );

    printf(
        "Inference IPC: %s\n",
        INFERENCE_SOCKET_PATH
    );

    printf(
        "Camera FOV : %.2f deg\n",
        CAMERA_FOV_DEG
    );
    printf("Target radius: %.3f mm (%s)\n",
           target_body_radius_mm, WITNESS_TARGET_RADIUS_ENV);

    printf("========================================\n");


    /*
     * 생성한 Thread들이 계속 실행되도록
     * Main Thread는 여기서 기다린다.
     *
     * pthread_join()은 Thread를 합치는 것이 아니라
     * 해당 Thread가 종료될 때까지 기다리는 함수다.
     *
     * 현재 Sensor/V2V/Filter Thread는
     * 무한 루프를 사용하므로 정상적인 상황에서는
     * 이 함수들이 반환되지 않는다.
     */
    pthread_join(
        sensor_thread,
        NULL
    );

    pthread_join(
        v2v_thread,
        NULL
    );

    pthread_join(
        filter_worker,
        NULL
    );


    ace_watchdog_stop(&filter_watchdog);
    led_controller_destroy(&led);

    return EXIT_FAILURE;
}
