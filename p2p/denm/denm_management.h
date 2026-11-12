#ifndef DENM_MANAGEMENT_H
#define DENM_MANAGEMENT_H

#include <stdint.h>

#include "DENM.h"
#include "p2p_protocol.h"

int fill_management(
    DENM_t *denm,
    const accident_event_payload_t *event
);

#endif
