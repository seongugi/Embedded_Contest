"""Convert ONLY the current TrackingEngine result to GPS-like BLE observations.

No estimation, prediction, vehicle commands, or stale-pose retention lives here.
The existing pose estimator keeps all six pose freedoms, including height.
"""
import math
import numpy as np


def load_mounts(config):
    """Return unit forward axes and position offsets in marker-local axes."""
    mounts = {}
    for car in config.get('cars', []):
        offset = np.asarray(car.get('target_offset_marker_mm', [0, 0, 0]), dtype=float)
        if offset.shape != (3,) or not np.isfinite(offset).all():
            raise ValueError('Each mount requires three finite offset values')
        if 'forward_axis_marker' in car:
            forward_axis = np.asarray(car['forward_axis_marker'], dtype=float)
            if forward_axis.shape != (3,) or not np.isfinite(forward_axis).all():
                raise ValueError('forward_axis_marker requires three finite values')
            scale = float(np.max(np.abs(forward_axis)))
            if scale == 0:
                raise ValueError('forward_axis_marker must be nonzero')
            # Scale before taking a norm to support finite large/small vectors.
            forward_axis = forward_axis / scale
            forward_axis = forward_axis / np.linalg.norm(forward_axis)
        else:
            angle = float(car.get('heading_offset_deg', 0))
            if not math.isfinite(angle):
                raise ValueError('heading_offset_deg must be finite')
            radians = math.radians(angle)
            forward_axis = np.array([math.cos(radians), math.sin(radians), 0.0])
        mounts[int(car['marker_id'])] = (forward_axis, offset)
    return mounts


def tracking_result_to_vehicles(result, mounts=None):
    """Convert measured poses once for both BLE transmission and the display."""
    vehicles = {}
    if result.get('camera') is not None:
        for marker_id, item in result.get('vehicles', {}).items():
            if marker_id not in range(1, 6):
                continue
            forward_axis, offset = (mounts or {}).get(
                marker_id, (np.array([1.0, 0.0, 0.0]), np.zeros(3)))
            rotation = np.asarray(item['pose'].R, dtype=float)
            center = np.asarray(item['xyz_mm'], dtype=float)
            if rotation.shape != (3, 3) or center.shape != (3,):
                continue
            if not np.isfinite(rotation).all() or not np.isfinite(center).all():
                continue
            # Use the full measured 3-D rotation: yaw addition would be wrong
            # for a tilted marker or an out-of-plane camera optical axis.
            forward = rotation @ forward_axis
            target = center + rotation @ offset
            if math.hypot(float(forward[0]), float(forward[1])) < 1e-8:
                continue  # A vertical forward axis has no defined planar heading.
            heading = math.degrees(math.atan2(float(forward[0]), float(forward[1]))) % 360.0
            vehicles[marker_id] = {'x_mm': float(target[0]), 'y_mm': float(target[1]),
                                   'heading_deg': heading}
    return vehicles


def publish_tracking_result(sender, result, frame_sequence, capture_mono_ns, mounts=None):
    vehicles = tracking_result_to_vehicles(result, mounts)
    reason = 'no_reference' if result.get('camera') is None else 'not_detected'
    sender.publish_frame(frame_sequence, capture_mono_ns, vehicles, reason=reason)
    return vehicles


def draw_ble_overlay(canvas, snapshot):
    """BLE progress uses an unobtrusive panel in the source frame's top right."""
    import cv2
    width = min(1200, canvas.shape[1] - 40)
    left = max(20, canvas.shape[1] - width - 20)
    cv2.rectangle(canvas, (left, 20), (left + width, 460), (25, 25, 25), -1)
    cv2.putText(canvas, 'BLE: measured position sender', (left+20, 65),
                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (230, 230, 230), 2)
    cars = snapshot.get('cars', {})
    if isinstance(cars, list):
        cars = {item['marker_id']: item for item in cars}
    for row, (marker_id, state) in enumerate(sorted(cars.items(), key=lambda item: int(item[0]))):
        status = str(state.get('state', state.get('status', 'STARTING')))
        sent = state.get('sent_pose', 0)
        line = f'ID {int(marker_id):02d}: {status} | poses {sent}'
        if status != 'ready' and state.get('last_error'):
            line += ' | ' + str(state['last_error']).encode('ascii', 'replace').decode()
        cv2.putText(canvas, line[:88], (left+20, 115+row*55),
                    cv2.FONT_HERSHEY_SIMPLEX, .92, (210, 235, 210), 2)
    error = str(snapshot.get('fatal_error') or '')
    if error:
        cv2.putText(canvas, error.encode('ascii', 'replace').decode()[:88],
                    (left+20, 430), cv2.FONT_HERSHEY_SIMPLEX, .7, (0, 170, 255), 2)
