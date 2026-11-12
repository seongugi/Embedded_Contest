#ifndef DENM_HEADER_H
#define DENM_HEADER_H

#include "DENM.h"
#include "p2p_protocol.h"

int fill_header(
    DENM_t *denm,
    const accident_event_payload_t *event
);

#endif
