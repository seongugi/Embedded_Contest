#include "denm_decoder.h"

#include <stdio.h>

#include "per_decoder.h"

int decode_denm(
    const uint8_t *payload,
    size_t payload_size,
    DENM_t **decoded_denm
)
{
    if (
        payload == NULL ||
        decoded_denm == NULL
    )
    {
        return -1;
    }

    *decoded_denm = NULL;

    asn_dec_rval_t decode_result =
        uper_decode_complete(
            NULL,
            &asn_DEF_DENM,
            (void **)decoded_denm,
            payload,
            payload_size
        );

    if (decode_result.code != RC_OK)
    {
        fprintf(
            stderr,
            "DENM 디코딩 실패: code=%d, consumed=%zu\n",
            decode_result.code,
            decode_result.consumed
        );

        if (*decoded_denm != NULL)
        {
            ASN_STRUCT_FREE(
                asn_DEF_DENM,
                *decoded_denm
            );

            *decoded_denm = NULL;
        }

        return -1;
    }

    return 0;
}
