#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include "../witness_fov.h"
#include "../witness_radius_config.h"

static witness_fov_result_t polar(double bearing, double distance,
                                  double heading, double radius)
{
    witness_fov_result_t result;
    double angle = bearing * WITNESS_PI / 180.0;
    assert(witness_evaluate_body_fov(0.0, 0.0, heading,
           distance * sin(angle), distance * cos(angle), radius, &result) == 0);
    return result;
}

static void test_center_and_visible_edge(void)
{
    witness_fov_result_t point = polar(36.0, 1000.0, 0.0, 0.0);
    witness_fov_result_t body = polar(36.0, 1000.0, 0.0, 100.0);
    assert(!point.candidate && body.candidate);
    assert(fabs(body.distance_mm - 1000.0) < 1e-9);
    assert(fabs(body.body_half_angle_deg - 5.739170477266787) < 1e-9);
    assert(!polar(38.0, 1000.0, 0.0, 100.0).candidate);
    assert(!polar(180.0, 1000.0, 0.0, 100.0).candidate);

    /* Every original in-FOV center remains a candidate, at both edges too. */
    for (int heading = 0; heading < 360; heading += 15) {
        for (int offset = -32; offset <= 32; ++offset) {
            assert(polar(heading + offset, 1000.0, heading, 100.0).candidate);
            assert(polar(heading + offset, 1000.0, heading, 0.0).candidate);
        }
    }
}

static void test_tangency_and_wrap(void)
{
    double radius = 1000.0 * sin(10.0 * WITNESS_PI / 180.0);
    assert(polar(42.0, 1000.0, 0.0, radius).candidate);
    assert(polar(-42.0, 1000.0, 0.0, radius).candidate);
    assert(!polar(42.000001, 1000.0, 0.0, radius).candidate);
    assert(!polar(-42.000001, 1000.0, 0.0, radius).candidate);
    assert(polar(1.0, 1000.0, 359.0, 0.0).candidate);
    assert(polar(359.0, 1000.0, 1.0, 0.0).candidate);
    assert(polar(36.0, 1000.0, 360.0, 100.0).candidate);
    assert(polar(36.0, 1000.0, -360.0, 100.0).candidate);
}

static void test_close_and_coincident(void)
{
    witness_fov_result_t result;
    assert(witness_evaluate_body_fov(0, 0, 0, 0, -100, 100, &result) == 0);
    assert(result.observer_inside_body && result.candidate);
    assert(polar(180.0, 99.0, 0.0, 100.0).candidate);
    assert(!polar(180.0, 100.001, 0.0, 100.0).candidate);
    assert(witness_evaluate_body_fov(7, 8, 180, 7, 8, 100, &result) == 0);
    assert(result.distance_mm == 0.0 && result.candidate);
    assert(witness_evaluate_body_fov(7, 8, 180, 7, 8, 0, &result) == 0);
    assert(result.candidate && isfinite(result.candidate_half_angle_deg));
}

static void test_coordinates_units_and_symmetry(void)
{
    witness_fov_result_t result;
    assert(witness_evaluate_body_fov(200, -300, 90, 1200, -300, 100, &result) == 0);
    assert(result.candidate && result.bearing_deg == 90.0);
    assert(result.distance_mm == 1000.0 && result.heading_error_deg == 0.0);
    assert(witness_evaluate_body_fov(200, -300, 0, 200, 700, 100, &result) == 0);
    assert(result.candidate && result.bearing_deg == 0.0);
    for (int angle = 0; angle <= 180; ++angle) {
        witness_fov_result_t left = polar(-angle, 1000, 0, 100);
        witness_fov_result_t right = polar(angle, 1000, 0, 100);
        witness_fov_result_t scaled = polar(angle, 10000, 0, 1000);
        assert(left.candidate == right.candidate);
        assert(right.candidate == scaled.candidate);
    }
    assert(polar(36, 1000, 0, 100).candidate);
    assert(!polar(36, 10000, 0, 100).candidate);
}

static void test_invalid_geometry(void)
{
    witness_fov_result_t result = {0};
    result.distance_mm = 123.0;
    assert(witness_evaluate_body_fov(NAN, 0, 0, 0, 0, 1, &result) == -1);
    assert(witness_evaluate_body_fov(0, INFINITY, 0, 0, 0, 1, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, NAN, 0, 0, 1, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, 0, NAN, 0, 1, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, 0, 0, INFINITY, 1, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, 0, 0, 0, NAN, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, 0, 0, 0, INFINITY, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, 0, 0, 0, -1, &result) == -1);
    assert(witness_evaluate_body_fov(-DBL_MAX, 0, 0, DBL_MAX, 0, 1, &result) == -1);
    assert(witness_evaluate_body_fov(0, 0, 0, 0, 0, 1, NULL) == -1);
    assert(result.distance_mm == 123.0);
}

static void test_radius_configuration(void)
{
    const char *invalid[] = {"", " ", "nan", "NaN", "inf", "-inf", "-1",
                             "100mm", "100 200", "100 ", "1e999", "1e-999"};
    double radius = -1;
    assert(witness_parse_target_radius_mm(NULL, WITNESS_DEFAULT_TARGET_RADIUS_MM,
                                         &radius) == 0);
    assert(fabs(radius * radius - (128.75 * 128.75 + 52.5 * 52.5)) < 1e-9);
    assert(witness_parse_target_radius_mm(NULL, 123.5, &radius) == 0 && radius == 123.5);
    assert(witness_parse_target_radius_mm("100.5", 1, &radius) == 0 && radius == 100.5);
    assert(witness_parse_target_radius_mm("1e2", 1, &radius) == 0 && radius == 100);
    assert(witness_parse_target_radius_mm("0", 1, &radius) == 0 && radius == 0);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        radius = 123;
        assert(witness_parse_target_radius_mm(invalid[i], 1, &radius) == -1);
        assert(radius == 123);
    }
    assert(witness_parse_target_radius_mm(NULL, NAN, &radius) == -1);
    assert(witness_parse_target_radius_mm(NULL, -1, &radius) == -1);
    assert(witness_parse_target_radius_mm("100", 1, NULL) == -1);
}

int main(void)
{
    test_center_and_visible_edge();
    test_tangency_and_wrap();
    test_close_and_coincident();
    test_coordinates_units_and_symmetry();
    test_invalid_geometry();
    test_radius_configuration();
    puts("PASS: witness body FOV geometry and radius configuration (6 groups)");
    return 0;
}
