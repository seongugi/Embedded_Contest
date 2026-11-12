#ifndef DENM_DECODER_H
#define DENM_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "DENM.h"

int decode_denm(
    const uint8_t *payload,
    size_t payload_size,
    DENM_t **decoded_denm
);

#endif
