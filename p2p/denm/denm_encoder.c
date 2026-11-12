#include "denm_encoder.h"

#include <stdio.h>

#include "constraints.h"
#include "per_encoder.h"

int encode_denm(
    const DENM_t *denm,
    uint8_t *encoded_buffer,
    size_t encoded_buffer_size,
    size_t *encoded_bytes,
    ssize_t *encoded_bits
)
{
    if (
        denm == NULL ||
        encoded_buffer == NULL ||
        encoded_bytes == NULL ||
        encoded_bits == NULL
    )
    {
        return -1;
    }

    char constraint_error[256];
    size_t constraint_error_length =
        sizeof(constraint_error);

    if (
        asn_check_constraints(
            &asn_DEF_DENM,
            denm,
            constraint_error,
            &constraint_error_length
        ) != 0
    )
    {
        fprintf(
            stderr,
            "DENM 제약조건 검사 실패: %s\n",
            constraint_error
        );

        return -1;
    }

    asn_enc_rval_t encode_result =
        uper_encode_to_buffer(
            &asn_DEF_DENM,
            denm,
            encoded_buffer,
            encoded_buffer_size
        );

    if (encode_result.encoded == -1)
    {
        fprintf(
            stderr,
            "DENM 인코딩 실패: %s\n",
            encode_result.failed_type != NULL
                ? encode_result.failed_type->name
                : "알 수 없는 타입"
        );

        return -1;
    }

    *encoded_bits = encode_result.encoded;
    *encoded_bytes =
        ((size_t)encode_result.encoded + 7U) / 8U;

    return 0;
}
