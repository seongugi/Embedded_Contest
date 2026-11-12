#include "btp.h"

#include <arpa/inet.h>
#include <string.h>

size_t btp_build_packet(
    uint8_t *packet,
    size_t packet_size,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_size
)
{
    btp_header_t header;

    size_t total_size =
        sizeof(header) + payload_size;

    if (packet == NULL)
    {
        return 0;
    }

    if (payload_size > 0 && payload == NULL)
    {
        return 0;
    }

    if (packet_size < total_size)
    {
        return 0;
    }

    header.destination_port =
        htons(destination_port);

    header.destination_port_info =
        htons(0);

    memcpy(
        packet,
        &header,
        sizeof(header)
    );

    if (payload_size > 0)
    {
        memcpy(
            packet + sizeof(header),
            payload,
            payload_size
        );
    }

    return total_size;
}

const uint8_t *btp_parse_packet(
    const uint8_t *packet,
    size_t packet_size,
    uint16_t *destination_port,
    size_t *payload_size
)
{
    btp_header_t header;

    if (
        packet == NULL ||
        destination_port == NULL ||
        payload_size == NULL
    )
    {
        return NULL;
    }

    if (packet_size < sizeof(btp_header_t))
    {
        return NULL;
    }

    /* 정렬되지 않은 네트워크 버퍼를 직접 구조체 포인터로 캐스팅하지 않는다. */
    memcpy(
        &header,
        packet,
        sizeof(header)
    );

    *destination_port =
        ntohs(header.destination_port);

    *payload_size =
        packet_size - sizeof(btp_header_t);

    return packet + sizeof(btp_header_t);
}
