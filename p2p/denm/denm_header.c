#include "denm_header.h"

#include "MessageId.h"

int fill_header(
    DENM_t *denm,
    const accident_event_payload_t *event
)
{
    denm->header.protocolVersion = 2;
    denm->header.messageId = MessageId_denm;
    denm->header.stationId = event->station_id;

    return 0;
}
