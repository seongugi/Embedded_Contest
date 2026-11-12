#ifndef ACE_WITNESS_FOV_H
#define ACE_WITNESS_FOV_H

#include <math.h>
#include <stddef.h>

#define WITNESS_CAMERA_FOV_DEG 64.0
#define WITNESS_PI 3.14159265358979323846

typedef struct {
    double distance_mm;
    double bearing_deg;
    double heading_error_deg;
    double body_half_angle_deg;
    double candidate_half_angle_deg;
    int observer_inside_body;
    int candidate;
} witness_fov_result_t;

/* +Y = 0 degrees, +X = 90 degrees; coordinates and body radius are all mm.
 * The disk must enclose the target body around the reported accident position.
 * Its angular extent only expands the first-stage candidate gate. It does not
 * change the camera's physical FOV or decide whether the image contains a car.
 * Returns -1 for invalid input and leaves the output untouched.
 */
static inline int witness_evaluate_body_fov(
    double observer_x_mm, double observer_y_mm, double observer_heading_deg,
    double target_x_mm, double target_y_mm, double target_radius_mm,
    witness_fov_result_t *result)
{
    witness_fov_result_t value;
    double dx, dy, heading;

    if (result == NULL || !isfinite(observer_x_mm) ||
        !isfinite(observer_y_mm) || !isfinite(observer_heading_deg) ||
        !isfinite(target_x_mm) || !isfinite(target_y_mm) ||
        !isfinite(target_radius_mm) || target_radius_mm < 0.0)
        return -1;

    dx = target_x_mm - observer_x_mm;
    dy = target_y_mm - observer_y_mm;
    value.distance_mm = hypot(dx, dy);
    if (!isfinite(value.distance_mm))
        return -1;

    heading = fmod(observer_heading_deg, 360.0);
    value.bearing_deg = atan2(dx, dy) * 180.0 / WITNESS_PI;
    if (value.bearing_deg < 0.0)
        value.bearing_deg += 360.0;
    value.heading_error_deg = fabs(remainder(value.bearing_deg - heading, 360.0));
    value.observer_inside_body = value.distance_mm <= target_radius_mm;

    if (value.observer_inside_body) {
        /* Includes coincident positions (d=0): never evaluate asin(R/d). */
        value.body_half_angle_deg = 180.0;
        value.candidate_half_angle_deg = 180.0;
        value.candidate = 1;
    } else {
        value.body_half_angle_deg =
            asin(target_radius_mm / value.distance_mm) * 180.0 / WITNESS_PI;
        value.candidate_half_angle_deg =
            WITNESS_CAMERA_FOV_DEG / 2.0 + value.body_half_angle_deg;
        /* Roundoff tolerance is one billionth of a degree, including tangency. */
        value.candidate = value.heading_error_deg <=
            value.candidate_half_angle_deg + 1e-9;
    }

    *result = value;
    return 0;
}

#endif
