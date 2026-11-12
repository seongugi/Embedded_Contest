#include "denm_situation.h"

#include <stdio.h>
#include <stdlib.h>

#include "CauseCodeChoice.h"
#include "AccidentSubCauseCode.h"

int fill_situation(DENM_t *denm)
{
    denm->denm.situation = calloc(
        1,
        sizeof(*denm->denm.situation)
    );

    if (denm->denm.situation == NULL)
    {
        perror("calloc");
        return -1;
    }

    SituationContainer_t *situation =
        denm->denm.situation;

    situation->informationQuality = 5;

    situation->eventType.ccAndScc.present =
        CauseCodeChoice_PR_accident2;

    situation->eventType.ccAndScc.choice.accident2 =
        AccidentSubCauseCode_multiVehicleAccident;

    situation->linkedCause = NULL;
    situation->eventZone = NULL;

    return 0;
}
