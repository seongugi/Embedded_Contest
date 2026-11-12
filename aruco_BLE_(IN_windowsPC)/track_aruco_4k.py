"""4K / 48mm ArUco tracking with live ID06+07 world registration.

World origin is the testbed's bottom-left physical corner. No size switch or lock.
Dependencies: numpy and the project's existing OpenCV (tested with 5.0.0).
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
import threading
import time

from tracking_ble_bridge import (publish_tracking_result, tracking_result_to_vehicles,
                                 draw_ble_overlay, load_mounts)
from aruco_ble_protocol import heading_centidegrees

import cv2
import numpy as np

MARKER_SIZE_M = 0.048
VEHICLE_IDS = frozenset(range(1, 6))
REFERENCE_IDS = frozenset((6, 7))


def get_marker_obj_points(marker_size_m=MARKER_SIZE_M):
    half = marker_size_m / 2.0
    return np.array([[-half, half, 0], [half, half, 0],
                     [half, -half, 0], [-half, -half, 0]], dtype=np.float64)


def rotation_distance_deg(a, b):
    delta, _ = cv2.Rodrigues(a.T @ b)
    return float(np.degrees(np.linalg.norm(delta)))


def interpolate_rotation(previous, current, alpha):
    """Shortest relative rotation on SO(3); safe across +/-180-degree rvecs."""
    delta, _ = cv2.Rodrigues(previous.T @ current)
    step, _ = cv2.Rodrigues(alpha * delta)
    return previous @ step


def reprojection_rms(obj, img, rvec, tvec, camera_matrix, distortion):
    projected, _ = cv2.projectPoints(obj, rvec, tvec, camera_matrix, distortion)
    errors = projected.reshape(-1, 2) - np.asarray(img).reshape(-1, 2)
    return float(np.sqrt(np.mean(np.sum(errors * errors, axis=1))))


@dataclass
class Pose:
    R: np.ndarray
    t: np.ndarray
    rms_px: float = 0.0

    @property
    def rvec(self):
        return cv2.Rodrigues(self.R)[0]


class AdaptivePoseFilter:
    """Time-aware low-pass filter: quiet when stationary, faster while moving.

    State is used only across consecutive valid observations. This is smoothing,
    never a held/predicted output for a missing marker.
    """
    def __init__(self, min_cutoff_hz=2.0, translation_beta=10.0,
                 rotation_beta=0.08, max_gap_seconds=0.2):
        self.min_cutoff = float(min_cutoff_hz)
        self.t_beta = float(translation_beta)
        self.r_beta = float(rotation_beta)
        self.max_gap = float(max_gap_seconds)
        self.reset()

    def reset(self):
        self.filtered = self.previous_raw = None
        self.last_time = self.last_step = None
        self.velocity = np.zeros((3, 1))
        self.angular_velocity = np.zeros((3, 1))

    @staticmethod
    def alpha(cutoff, dt):
        return 1.0 / (1.0 + 1.0 / (2.0 * math.pi * cutoff * dt))

    def update(self, pose, timestamp, step):
        dt = None if self.last_time is None else timestamp - self.last_time
        fresh = (self.filtered is None or step != self.last_step + 1
                 or dt is None or dt <= 0 or dt > self.max_gap)
        if fresh:
            self.reset()
            self.filtered = Pose(pose.R.copy(), pose.t.copy(), pose.rms_px)
        else:
            derivative_alpha = self.alpha(1.0, dt)
            velocity = (pose.t - self.previous_raw.t) / dt
            # Express the angular increment in the shared parent coordinates.
            angular, _ = cv2.Rodrigues(pose.R @ self.previous_raw.R.T)
            angular = np.degrees(angular) / dt
            self.velocity += derivative_alpha * (velocity - self.velocity)
            self.angular_velocity += derivative_alpha * (angular - self.angular_velocity)
            at = self.alpha(self.min_cutoff + self.t_beta * np.linalg.norm(self.velocity), dt)
            ar = self.alpha(self.min_cutoff + self.r_beta * np.linalg.norm(self.angular_velocity), dt)
            self.filtered = Pose(
                interpolate_rotation(self.filtered.R, pose.R, ar),
                self.filtered.t + at * (pose.t - self.filtered.t), pose.rms_px)
        self.previous_raw = Pose(pose.R.copy(), pose.t.copy(), pose.rms_px)
        self.last_time, self.last_step = timestamp, step
        return self.filtered


def solve_pose(obj, img, K, D, previous=None, square=False,
               max_rms=2.0, ambiguous_band=0.15):
    """Evaluate both planar pose solutions before using temporal continuity."""
    obj = np.ascontiguousarray(obj, dtype=np.float64).reshape(-1, 3)
    img = np.ascontiguousarray(img, dtype=np.float64).reshape(-1, 2)
    if not np.isfinite(img).all() or not np.isfinite(obj).all():
        return None
    planar = np.linalg.matrix_rank(obj - obj.mean(axis=0), tol=1e-7) <= 2
    flag = cv2.SOLVEPNP_IPPE_SQUARE if square else (
        cv2.SOLVEPNP_IPPE if planar else cv2.SOLVEPNP_SQPNP)
    try:
        solutions = cv2.solvePnPGeneric(obj, img, K, D, flags=flag)
    except cv2.error:
        return None
    candidates = []
    for rv, tv in zip(solutions[1], solutions[2]):
        if not np.isfinite(rv).all() or not np.isfinite(tv).all():
            continue
        R = cv2.Rodrigues(rv)[0]
        if np.min((R @ obj.T + tv.reshape(3, 1))[2]) <= 0.01:
            continue
        rms = reprojection_rms(obj, img, rv, tv, K, D)
        if math.isfinite(rms):
            candidates.append(Pose(R, tv.reshape(3, 1), rms))
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.rms_px)
    chosen = candidates[0]
    if previous is not None:
        plausible = [p for p in candidates if p.rms_px <= chosen.rms_px + ambiguous_band]
        chosen = min(plausible, key=lambda p:
            rotation_distance_deg(previous.R, p.R) + 100 * np.linalg.norm(previous.t - p.t))
    # Refine against this frame's actual observations, not filtered corners.
    try:
        rv, tv = cv2.solvePnPRefineLM(obj, img, K, D, chosen.rvec.copy(), chosen.t.copy())
        refined = Pose(cv2.Rodrigues(rv)[0], tv.reshape(3, 1),
                       reprojection_rms(obj, img, rv, tv, K, D))
        if (np.isfinite(refined.R).all() and np.isfinite(refined.t).all()
                and np.min((refined.R @ obj.T + refined.t)[2]) > 0.01
                and refined.rms_px <= chosen.rms_px + 1e-6):
            chosen = refined
    except cv2.error:
        pass
    return chosen if chosen.rms_px <= max_rms else None


def reference_world_points(config):
    result = {}
    local = get_marker_obj_points()
    for marker_id in sorted(REFERENCE_IDS):
        entry = config['reference_markers'][str(marker_id)]
        center = np.asarray(entry['center_mm'], dtype=float).reshape(3) / 1000.0
        yaw = np.deg2rad(float(entry['yaw_deg']))
        R = np.array([[np.cos(yaw), -np.sin(yaw), 0],
                      [np.sin(yaw), np.cos(yaw), 0], [0, 0, 1]])
        if not np.isfinite(center).all() or not np.isfinite(R).all():
            raise ValueError('Reference placement must contain finite measurements.')
        result[marker_id] = local @ R.T + center
    if np.linalg.norm(result[6].mean(axis=0) - result[7].mean(axis=0)) < MARKER_SIZE_M:
        raise ValueError('Reference marker centers must be distinct.')
    return result


class TrackingEngine:
    def __init__(self, K, D, config):
        self.K, self.D, self.config = K, D, config
        self.reference_points = reference_world_points(config)
        self.camera_filter = self.make_filter('reference_filter')
        self.vehicle_filters = {}
        self.camera_previous = None
        self.vehicle_camera_previous = {}
        self.step = 0
        self.last_time = None

    def make_filter(self, which):
        return AdaptivePoseFilter(**self.config[which],
            max_gap_seconds=self.config.get('max_filter_gap_seconds', 0.2))

    def clear(self):
        self.camera_filter.reset()
        self.camera_previous = None
        self.vehicle_filters.clear()
        self.vehicle_camera_previous.clear()

    def estimate(self, obj, img, previous=None, square=False, max_rms=None):
        return solve_pose(obj, img, self.K, self.D, previous, square,
            self.config.get('max_reprojection_rms_px', 2.0) if max_rms is None else max_rms,
            self.config.get('ambiguous_error_band_px', 0.15))

    def estimate_reference(self, observations):
        """Use current ID06 as the anchor and ID07 as an optional correction."""
        limit = float(self.config.get('reference_max_reprojection_rms_px', 6.0))
        primary = None
        if 6 in observations:
            primary = self.estimate(self.reference_points[6], observations[6],
                                    self.camera_previous, max_rms=limit)
        if primary is None:
            if 7 in observations:
                secondary = self.estimate(self.reference_points[7], observations[7],
                                          self.camera_previous, max_rms=limit)
                if secondary is not None:
                    return secondary, [7]
            return None, []
        if 7 not in observations:
            return primary, [6]

        # Repeating ID06's rows gives it twice ID07's least-squares weight.
        # Both the planar solve and LM refinement keep this 2:1 weighting.
        obj = np.concatenate([self.reference_points[i] for i in (6, 6, 7)])
        img = np.concatenate([observations[i] for i in (6, 6, 7)])
        corrected = self.estimate(obj, img, primary, max_rms=limit)
        if corrected is not None:
            errors = [reprojection_rms(self.reference_points[i], observations[i],
                        corrected.rvec, corrected.t, self.K, self.D) for i in (6, 7)]
            # The primary's extra weight must not hide a bad fit to ID07.
            if all(error <= limit for error in errors):
                corrected.rms_px = float(np.sqrt(np.mean(np.square(errors))))
                return corrected, [6, 7]
        # A failed correction never discards this frame's valid ID06 pose.
        return primary, [6]

    def process(self, observations, timestamp):
        self.step += 1
        if self.last_time is not None and (timestamp <= self.last_time or
                timestamp - self.last_time > self.config.get('max_filter_gap_seconds', 0.2)):
            self.clear()
        self.last_time = timestamp
        visible_refs = sorted(REFERENCE_IDS.intersection(observations))
        result = {'references': visible_refs, 'camera': None, 'vehicles': {},
                  'status': 'NO REFERENCE', 'camera_changed': False}
        if not visible_refs:
            self.clear()
            return result
        camera, accepted_refs = self.estimate_reference(observations)
        if camera is None:
            self.clear()
            result['status'] = 'REFERENCE QUALITY LOW'
            return result
        # A real camera displacement must not slowly drag the world's origin.
        if self.camera_previous is not None and (
                np.linalg.norm(camera.t - self.camera_previous.t) > 0.05 or
                rotation_distance_deg(camera.R, self.camera_previous.R) > 3.0):
            self.clear()
            result['camera_changed'] = True
        self.camera_previous = camera
        active_camera = self.camera_filter.update(camera, timestamp, self.step)
        result['references'] = accepted_refs
        result['camera'] = active_camera
        result['reference_rms_px'] = camera.rms_px
        result['status'] = 'DUAL REF 06+07' if len(accepted_refs) == 2 else f'SINGLE REF {accepted_refs[0]:02d}'

        # A missed/invalid observation deletes its state immediately.
        accepted_filters, accepted_previous = {}, {}
        for marker_id in sorted(VEHICLE_IDS.intersection(observations)):
            raw = self.estimate(get_marker_obj_points(), observations[marker_id],
                                self.vehicle_camera_previous.get(marker_id), square=True)
            if raw is None:
                continue
            world = Pose(active_camera.R.T @ raw.R,
                         active_camera.R.T @ (raw.t - active_camera.t), raw.rms_px)
            filt = self.vehicle_filters.get(marker_id) or self.make_filter('vehicle_filter')
            smoothed = filt.update(world, timestamp, self.step)
            accepted_filters[marker_id] = filt
            accepted_previous[marker_id] = raw
            yaw = float(np.degrees(np.arctan2(smoothed.R[1, 0], smoothed.R[0, 0])))
            result['vehicles'][marker_id] = {'pose': smoothed, 'raw_world_pose': world,
                'camera_pose': raw, 'xyz_mm': smoothed.t.ravel() * 1000,
                'yaw_deg': yaw, 'rms_px': raw.rms_px}
        self.vehicle_filters = accepted_filters
        self.vehicle_camera_previous = accepted_previous
        return result


def unique_observations(corners, ids):
    if ids is None:
        return {}
    result, duplicates = {}, set()
    for corner, value in zip(corners, np.asarray(ids).ravel()):
        marker_id = int(value)
        if marker_id not in VEHICLE_IDS | REFERENCE_IDS:
            continue
        if marker_id in result:
            duplicates.add(marker_id)
        result[marker_id] = np.asarray(corner, dtype=float).reshape(4, 2)
    for marker_id in duplicates:
        result.pop(marker_id, None)
    return result


class LatestFrameCamera:
    """Continuously drain capture; consumer receives only the newest frame."""
    def __init__(self, index):
        self.index = index
        self.condition = threading.Condition()
        self.stop = threading.Event()
        self.latest = None
        self.error = None
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    def _run(self):
        cap = None
        try:
            cap = cv2.VideoCapture(self.index, cv2.CAP_DSHOW)
            if not cap.isOpened():
                cap.release()
                cap = cv2.VideoCapture(self.index)
            if not cap.isOpened():
                raise RuntimeError(f'Cannot open camera {self.index}.')
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, 3840)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 2160)
            # Validate the negotiated format once, before any frame reaches the
            # 4K calibration. Keep the capture loop free of resolution checks.
            ok, frame = cap.read()
            if not ok:
                raise RuntimeError('Camera frame read failed.')
            capture_mono, capture_wall = time.monotonic(), time.time()
            height, width = frame.shape[:2]
            if (width, height) != (3840, 2160):
                raise RuntimeError(
                    f'Camera returned {width}x{height}; 4K calibration requires 3840x2160.')
            sequence = 1
            with self.condition:
                self.latest = (sequence, capture_mono, capture_wall, frame)
                self.condition.notify_all()
            while not self.stop.is_set():
                ok, frame = cap.read()
                if not ok:
                    raise RuntimeError('Camera frame read failed.')
                sequence += 1
                # This is host read-completion time, not camera exposure time.
                packet = (sequence, time.monotonic(), time.time(), frame)
                with self.condition:
                    self.latest = packet
                    self.condition.notify_all()
        except Exception as exc:
            with self.condition:
                self.error = str(exc)
                self.condition.notify_all()
        finally:
            if cap is not None:
                cap.release()

    def get(self, after_sequence):
        with self.condition:
            self.condition.wait_for(lambda: self.error or self.stop.is_set() or (
                self.latest is not None and self.latest[0] > after_sequence), timeout=0.5)
            if self.error:
                raise RuntimeError(self.error)
            if self.latest is None or self.latest[0] <= after_sequence:
                return None
            return self.latest

    def close(self):
        self.stop.set()
        with self.condition:
            self.condition.notify_all()
        self.worker.join(timeout=2.0)


def make_detector():
    params = cv2.aruco.DetectorParameters()
    params.cornerRefinementMethod = cv2.aruco.CORNER_REFINE_SUBPIX
    params.cornerRefinementWinSize = 5
    params.cornerRefinementMaxIterations = 30
    params.cornerRefinementMinAccuracy = 0.02
    params.minDistanceToBorder = 3
    params.adaptiveThreshWinSizeMin = 3
    params.adaptiveThreshWinSizeMax = 35
    params.adaptiveThreshWinSizeStep = 10
    params.minMarkerPerimeterRate = 0.012
    return cv2.aruco.ArucoDetector(
        cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_4X4_50), params)


def draw_overlay(frame, observations, result, K, D, table_size, show_table, logging,
                 processing_fps, host_age_ms, vehicle_poses):
    canvas = frame.copy()
    camera = result['camera']
    if camera is not None and show_table:
        w, h = np.asarray(table_size) / 1000
        pts = np.array([[0, 0, 0], [w, 0, 0], [w, h, 0], [0, h, 0]], dtype=float)
        projected = cv2.projectPoints(pts, camera.rvec, camera.t, K, D)[0].reshape(-1, 2)
        if np.isfinite(projected).all() and np.max(np.abs(projected)) < 1e6:
            cv2.polylines(canvas, [projected.astype(np.int32)], True, (0, 220, 220), 3)
    for marker_id, corner in observations.items():
        color = (0, 220, 0) if marker_id in REFERENCE_IDS else (255, 140, 0)
        cv2.polylines(canvas, [corner.astype(np.int32)], True, color, 3)
        x, y = corner.mean(axis=0).astype(int)
        cv2.putText(canvas, f'ID {marker_id:02d}', (x-40, y-45),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.1, color, 2)
    cv2.rectangle(canvas, (20, 20), (1510, 480), (20, 20, 20), -1)
    color = (0, 220, 0) if len(result['references']) == 2 and camera is not None else (0, 180, 255)
    cv2.putText(canvas, f"48mm | {result['status']} | World origin: table bottom-left", (40, 65),
                cv2.FONT_HERSHEY_SIMPLEX, 1.05, color, 2)
    cv2.putText(canvas, f"Processing {processing_fps:.1f} FPS | Host frame age {host_age_ms:.0f}ms | CSV {'ON' if logging else 'OFF'}",
                (40, 110), cv2.FONT_HERSHEY_SIMPLEX, .95, (220, 220, 220), 2)
    for row, marker_id in enumerate(sorted(VEHICLE_IDS)):
        item = result['vehicles'].get(marker_id)
        if item is None:
            label = f'ID {marker_id:02d}: NO CURRENT VALID POSITION'
            ink = (140, 140, 140)
        else:
            x, y, z = item['xyz_mm']
            pose = vehicle_poses.get(marker_id)
            heading_text = f"{heading_centidegrees(pose['heading_deg']) / 100:7.2f} deg" if pose else "N/A"
            label = f"ID {marker_id:02d}: X={x:8.1f} Y={y:8.1f} Z={z:7.1f} mm | Heading={heading_text}"
            ink = (245, 245, 245)
        cv2.putText(canvas, label, (40, 165 + 53*row), cv2.FONT_HERSHEY_SIMPLEX, .96, ink, 2)
    cv2.putText(canvas, '[Keys] T: outline | L: overwrite/start or stop CSV | Q/ESC: quit',
                (40, frame.shape[0]-40), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (220, 220, 220), 2)
    return canvas


def main(argv=None):
    folder = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description='48mm ArUco tracking; live reference IDs 06 and 07')
    parser.add_argument('--config', type=Path, default=folder/'tracking_config.json')
    parser.add_argument('--calibration', type=Path, default=folder/'camera_calibration_4k.npz')
    parser.add_argument('--ble', action='store_true', help='Send measured poses to configured BLE vehicles')
    parser.add_argument('--ble-config', type=Path, default=folder/'ble_config.json')
    args = parser.parse_args(argv)
    config = json.loads(args.config.read_text(encoding='utf-8-sig'))
    with np.load(args.calibration, allow_pickle=False) as data:
        K, D = data['camera_matrix'].copy(), data['dist_coeffs'].copy()
    engine = TrackingEngine(K, D, config)
    detector = make_detector()
    sender = None
    mounts = {}
    if args.ble_config.exists():
        mounts = load_mounts(json.loads(args.ble_config.read_text(encoding='utf-8-sig')))
    if args.ble:
        from aruco_ble_sender import BLEPoseSender, load_config
        ble_config = load_config(args.ble_config)
        mounts = load_mounts(ble_config)
        sender = BLEPoseSender(ble_config)
        sender.start()
    print('48mm fixed. Live reference IDs 06+07. No origin lock or M mode.')
    print('World (0,0): physical bottom-left table corner. Check reference centers in tracking_config.json.')
    print('Heading: world +Y = 0 deg, +X = 90 deg; forward axis from ble_config.json.')
    print('T: outline | L: overwrite/start/stop CSV | Q/ESC: quit')
    if sender is not None:
        print('BLE enabled: measured XY/heading only; no predictions or vehicle commands.')
        print('Frame timestamps mean host read completion; camera buffering is not included.')
    camera = LatestFrameCamera(int(config.get('camera_index', 1)))
    window = 'ArUco 48mm - live dual reference'
    csv_file = writer = None
    show_table = True
    last_sequence = 0
    last_processed = None
    try:
        cv2.namedWindow(window, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(window, 1280, 720)
        while True:
            packet = camera.get(last_sequence)
            if packet is None:
                engine.clear()
                if sender is not None:
                    sender.invalidate_all(reason='camera_unavailable')
                blank = np.zeros((720, 1280, 3), dtype=np.uint8)
                cv2.putText(blank, 'NO NEW CAMERA FRAME - POSITIONS INVALID', (40, 360),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 180, 255), 2)
                cv2.imshow(window, blank)
                if cv2.waitKey(1) & 0xFF in (ord('q'), 27):
                    break
                continue
            sequence, capture_mono, capture_wall, frame = packet
            last_sequence = sequence
            corners, ids, _ = detector.detectMarkers(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY))
            observations = unique_observations(corners, ids)
            result = engine.process(observations, capture_mono)
            if sender is not None:
                vehicle_poses = publish_tracking_result(sender, result, sequence,
                                        round(capture_mono * 1_000_000_000), mounts)
            else:
                vehicle_poses = tracking_result_to_vehicles(result, mounts)
            finished = time.monotonic()
            fps = 0 if last_processed is None else 1.0 / max(finished-last_processed, 1e-6)
            last_processed = finished
            if writer is not None:
                for marker_id in sorted(VEHICLE_IDS):
                    item = result['vehicles'].get(marker_id)
                    xyz = item['xyz_mm'].tolist() if item else ['', '', '']
                    pose = vehicle_poses.get(marker_id)
                    heading = heading_centidegrees(pose['heading_deg']) / 100 if pose else ''
                    writer.writerow([capture_wall, capture_mono, sequence, marker_id, bool(item),
                        *xyz, item['yaw_deg'] if item else '', item['rms_px'] if item else '',
                        '+'.join(map(str, result['references'])), result['status'],
                        heading])
                csv_file.flush()
            canvas = draw_overlay(frame, observations, result, K, D, config['table_size_mm'],
                show_table, writer is not None, fps, (finished-capture_mono)*1000, vehicle_poses)
            if sender is not None:
                draw_ble_overlay(canvas, sender.snapshot())
            cv2.imshow(window, canvas)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord('q'), ord('Q'), 27):
                break
            if key in (ord('t'), ord('T')):
                show_table = not show_table
            elif key in (ord('l'), ord('L')):
                if csv_file is not None:
                    csv_file.close()
                    csv_file = writer = None
                else:
                    csv_file = (folder/'marker_coordinates_log.csv').open('w', newline='', encoding='utf-8')
                    writer = csv.writer(csv_file)
                    writer.writerow(['Host_capture_unix_s', 'Host_capture_monotonic_s', 'Frame', 'Marker_ID',
                        'Valid', 'X_mm', 'Y_mm', 'Z_mm', 'Yaw_deg', 'Marker_RMS_px', 'Visible_refs', 'Reference_status', 'Heading_deg'])
    finally:
        if sender is not None:
            sender.invalidate_all(reason='sender_stopping')
            sender.stop()
        if csv_file is not None:
            csv_file.close()
        camera.close()
        cv2.destroyAllWindows()


if __name__ == '__main__':
    main()
