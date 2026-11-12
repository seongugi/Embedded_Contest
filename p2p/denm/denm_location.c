#include "denm_location.h"

#include <stdio.h>
#include <stdlib.h>

#include "SpeedConfidence.h"
#include "Wgs84AngleConfidence.h"
#include "Path.h"
#include "PathPoint.h"

int fill_location(
    DENM_t *denm,
    const accident_event_payload_t *event
)
{
    denm->denm.location = calloc(
        1,
        sizeof(*denm->denm.location)
    );

    if (denm->denm.location == NULL)
    {
        perror("calloc");
        return -1;
    }

    LocationContainer_t *location =
        denm->denm.location;

    location->eventSpeed = calloc(
        1,
        sizeof(*location->eventSpeed)
    );

    if (location->eventSpeed == NULL)
    {
        perror("calloc");
        return -1;
    }

    location->eventSpeed->speedValue =
        event->speed_value;

    location->eventSpeed->speedConfidence =
        SpeedConfidence_unavailable;

    location->eventPositionHeading = calloc(
        1,
        sizeof(*location->eventPositionHeading)
    );

    if (location->eventPositionHeading == NULL)
    {
        perror("calloc");
        return -1;
    }

    location->eventPositionHeading->value =
        event->heading_value;

    location->eventPositionHeading->confidence =
        Wgs84AngleConfidence_unavailable;

    Path_t *path = calloc(1, sizeof(*path));

    if (path == NULL)
    {
        perror("calloc");
        return -1;
    }

    PathPoint_t *point = calloc(1, sizeof(*point));

    if (point == NULL)
    {
        perror("calloc");
        free(path);
        return -1;
    }

    point->pathPosition.deltaLatitude =
        event->delta_latitude;

    point->pathPosition.deltaLongitude =
        event->delta_longitude;

    point->pathPosition.deltaAltitude =
        event->delta_altitude;

    point->pathDeltaTime = NULL;

    if (ASN_SEQUENCE_ADD(&path->list, point) != 0)
    {
        fprintf(stderr, "PathPoint 추가 실패\n");
        free(point);
        free(path);
        return -1;
    }

    if (ASN_SEQUENCE_ADD(&location->traces.list, path) != 0)
    {
        fprintf(stderr, "Path 추가 실패\n");
        ASN_STRUCT_FREE(asn_DEF_Path, path);
        return -1;
    }

    location->roadType = NULL;

    return 0;
}
