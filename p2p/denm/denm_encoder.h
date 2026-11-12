#ifndef DENM_ENCODER_H
#define DENM_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "DENM.h"

int encode_denm(
    const DENM_t *denm,
    uint8_t *encoded_buffer,
    size_t encoded_buffer_size,
    size_t *encoded_bytes,
    ssize_t *encoded_bits
);

#endif
