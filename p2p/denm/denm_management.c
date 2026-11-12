#include "denm_management.h"

#include <stdio.h>
#include <stdlib.h>

#include "StationType.h"
#include "SemiAxisLength.h"
#include "HeadingValue.h"
#include "AltitudeValue.h"
#include "AltitudeConfidence.h"
#include "INTEGER.h"

int fill_management(
    DENM_t *denm,
    const accident_event_payload_t *event
)
{
    ManagementContainer_t *management =
        &denm->denm.management;

    management->actionId.originatingStationId =
        event->station_id;

    management->actionId.sequenceNumber =
        (long)event->sequence_number;

    if (
        asn_long2INTEGER(
            &management->detectionTime,
            (long)event->detection_time
        ) != 0
    )
    {
        fprintf(stderr, "DetectionTime 설정 실패\n");
        return -1;
    }

    if (
        asn_long2INTEGER(
            &management->referenceTime,
            (long)event->reference_time
        ) != 0
    )
    {
        fprintf(stderr, "ReferenceTime 설정 실패\n");
        return -1;
    }

    management->eventPosition.latitude =
        event->latitude;

    management->eventPosition.longitude =
        event->longitude;

    management->eventPosition.
        positionConfidenceEllipse.
        semiMajorConfidence =
            SemiAxisLength_unavailable;

    management->eventPosition.
        positionConfidenceEllipse.
        semiMinorConfidence =
            SemiAxisLength_unavailable;

    management->eventPosition.
        positionConfidenceEllipse.
        semiMajorOrientation =
            HeadingValue_unavailable;

    management->eventPosition.
        altitude.
        altitudeValue =
            AltitudeValue_unavailable;

    management->eventPosition.
        altitude.
        altitudeConfidence =
            AltitudeConfidence_unavailable;

    management->validityDuration = calloc(
        1,
        sizeof(*management->validityDuration)
    );

    if (management->validityDuration == NULL)
    {
        perror("calloc");
        return -1;
    }

    *management->validityDuration = 60;

    management->stationType =
        StationType_passengerCar;

    return 0;
}
