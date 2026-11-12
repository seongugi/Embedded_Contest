#ifndef BTP_H
#define BTP_H

#include <stddef.h>
#include <stdint.h>

/*
 * BTP Destination Port
 *
 * CAM은 향후 확장을 위해 유지한다.
 * HEARTBEAT와 ACK는 프로젝트 내부 V2V 제어 메시지다.
 */
#define BTP_PORT_CAM        2001
#define BTP_PORT_DENM       2002
#define BTP_PORT_HEARTBEAT  3001
#define BTP_PORT_ACK        3002

/* Heartbeat payload 식별값과 타입 */
#define BTP_HEARTBEAT_MAGIC     0x48425431U
#define BTP_HEARTBEAT_REQUEST   1U
#define BTP_HEARTBEAT_RESPONSE  2U

/* 송신 노드 역할 */
#define BTP_ROLE_GO      1U
#define BTP_ROLE_CLIENT  2U

typedef struct
{
    uint16_t destination_port;
    uint16_t destination_port_info;

} __attribute__((packed)) btp_header_t;

/*
 * BTP_PORT_HEARTBEAT 뒤에 오는 payload다.
 * 모든 정수 필드는 네트워크 바이트 순서로 전송한다.
 */
typedef struct
{
    uint32_t magic;
    uint32_t type;
    uint32_t role;
    uint32_t sequence;

} __attribute__((packed)) btp_heartbeat_payload_t;

/*
 * BTP_PORT_ACK 뒤에 오는 payload다.
 * DENM ActionID.sequenceNumber와 대응되는 sequence를 담는다.
 */
typedef struct
{
    uint32_t sequence;

} __attribute__((packed)) btp_ack_payload_t;

size_t btp_build_packet(
    uint8_t *packet,
    size_t packet_size,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_size
);

const uint8_t *btp_parse_packet(
    const uint8_t *packet,
    size_t packet_size,
    uint16_t *destination_port,
    size_t *payload_size
);

#endif
