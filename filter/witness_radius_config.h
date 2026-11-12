#ifndef ACE_WITNESS_RADIUS_CONFIG_H
#define ACE_WITNESS_RADIUS_CONFIG_H

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#define WITNESS_TARGET_RADIUS_ENV "WITNESS_TARGET_RADIUS_MM"

/* Centered marker, 1:18 IONIQ 5: full-size body 4635 x 1890 mm.
 * The half diagonal encloses the 257.5 x 105 mm model in every heading.
 */
#define WITNESS_DEFAULT_TARGET_RADIUS_MM (hypot(4635.0 / 36.0, 1890.0 / 36.0))

/* Only an absent variable selects the caller's default. Explicit malformed
 * values must not silently change the configured target body size.
 * Zero is allowed for the original point-target gate; all lengths are mm.
 */
static inline int witness_parse_target_radius_mm(
    const char *text, double default_radius_mm, double *radius_mm)
{
    char *end;
    double value;

    if (radius_mm == NULL)
        return -1;
    if (text == NULL) {
        value = default_radius_mm;
    } else {
        errno = 0;
        value = strtod(text, &end);
        if (errno != 0 || end == text || *end != '\0')
            return -1;
    }
    if (!isfinite(value) || value < 0.0)
        return -1;

    *radius_mm = value;
    return 0;
}

#endif
