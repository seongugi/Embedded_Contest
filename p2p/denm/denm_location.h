#ifndef DENM_LOCATION_H
#define DENM_LOCATION_H

#include "DENM.h"
#include "p2p_protocol.h"

int fill_location(
    DENM_t *denm,
    const accident_event_payload_t *event
);

#endif
