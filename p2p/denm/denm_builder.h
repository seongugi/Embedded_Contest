#ifndef DENM_BUILDER_H
#define DENM_BUILDER_H

#include <stdint.h>

#include "DENM.h"
#include "p2p_protocol.h"

int fill_denm(
    DENM_t *denm,
    const accident_event_payload_t *event
);

#endif
