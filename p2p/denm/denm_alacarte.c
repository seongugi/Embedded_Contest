#include "denm_alacarte.h"

#include <stdio.h>
#include <stdlib.h>

#include "LocalCoordinates.h"

int fill_alacarte(
    DENM_t *denm,
    const accident_event_payload_t *event
)
{
    denm->denm.alacarte = calloc(
        1,
        sizeof(*denm->denm.alacarte)
    );

    if (denm->denm.alacarte == NULL)
    {
        perror("calloc");
        return -1;
    }

    AlacarteContainer_t *alacarte =
        denm->denm.alacarte;

    alacarte->lanePosition = NULL;
    alacarte->impactReduction = NULL;
    alacarte->externalTemperature = NULL;
    alacarte->roadWorks = NULL;
    alacarte->positioningSolution = NULL;
    alacarte->stationaryVehicle = NULL;

    alacarte->localCoordinates = calloc(
        1,
        sizeof(*alacarte->localCoordinates)
    );

    if (alacarte->localCoordinates == NULL)
    {
        perror("calloc");
        return -1;
    }

    alacarte->localCoordinates->xMillimeter =
        event->local_x_mm;

    alacarte->localCoordinates->yMillimeter =
        event->local_y_mm;

    return 0;
}
