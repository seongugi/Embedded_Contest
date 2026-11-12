#include "denm_builder.h"

#include "denm_header.h"
#include "denm_management.h"
#include "denm_situation.h"
#include "denm_location.h"
#include "denm_alacarte.h"

int fill_denm(
    DENM_t *denm,
    const accident_event_payload_t *event
)
{
    if (denm == NULL || event == NULL)
    {
        return -1;
    }

    if (fill_header(denm, event) != 0)
    {
        return -1;
    }

    if (fill_management(denm, event) != 0)
    {
        return -1;
    }

    if (fill_situation(denm) != 0)
    {
        return -1;
    }

    if (fill_location(denm, event) != 0)
    {
        return -1;
    }

    if (fill_alacarte(denm, event) != 0)
    {
        return -1;
    }

    return 0;
}
