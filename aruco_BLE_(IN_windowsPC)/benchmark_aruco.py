"""Measure this project's ArUco detector, without BLE or vehicle control."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time


def positive_int(value):
    value = int(value)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def positive_float(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return value


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--images", "--images-folder", type=Path, help="Preload images from this folder")
    source.add_argument("--camera-index", "--camera", type=int, help="Open camera explicitly; never opened in image mode")
    p.add_argument("--repeats", type=positive_int, default=3, help="Image-mode passes, default 3")
    p.add_argument("--max-images", type=positive_int, default=33)
    p.add_argument("--warmup", type=int, default=3, help="Excluded warmup frames, default 3")
    p.add_argument("--seconds", type=positive_float, default=30, help="Camera measurement duration")
    p.add_argument("--width", type=positive_int, default=3840)
    p.add_argument("--height", type=positive_int, default=2160)
    p.add_argument("--fps", type=positive_float, default=30, help="Requested camera FPS; measured FPS is separate")
    p.add_argument("--fourcc", default="MJPG", help="Requested camera format, default MJPG")
    p.add_argument("--backend", choices=("auto", "any", "dshow", "v4l2"), default="auto")
    p.add_argument("--allow-other-size", action="store_true", help="Image mode only: report mixed/nonrequested sizes instead of failing")
    p.add_argument("--threads", type=positive_int, help="Explicit OpenCV thread count; omitted leaves OpenCV default untouched")
    p.add_argument("--output", type=Path, help="Write JSON to this explicit path (overwrites); otherwise stdout only")
    return p


def platform_state():
    result = {"cpu_model": platform.processor() or None}
    if platform.system() == "Linux":
        try:
            lines = Path("/proc/cpuinfo").read_text(errors="replace").splitlines()
            for field in ("Model", "model name", "Hardware"):
                matches = [line.split(":", 1)[1].strip() for line in lines if line.split(":", 1)[0].strip() == field]
                if matches:
                    result[field.replace(" ", "_").lower()] = matches[0]
        except OSError as exc:
            result["cpuinfo_error"] = str(exc)
        temperatures = {}
        for zone in sorted(Path("/sys/class/thermal").glob("thermal_zone*")):
            try:
                temperatures[zone.name] = {
                    "type": (zone / "type").read_text().strip(),
                    "celsius": float((zone / "temp").read_text().strip()) / 1000.0,
                }
            except (OSError, ValueError):
                pass
        result["temperatures"] = temperatures
        command = shutil.which("vcgencmd")
        if command:
            try:
                completed = subprocess.run([command, "get_throttled"], capture_output=True, text=True, timeout=3, check=False)
                result["vcgencmd_get_throttled"] = completed.stdout.strip()
                result["vcgencmd_returncode"] = completed.returncode
            except (OSError, subprocess.TimeoutExpired) as exc:
                result["vcgencmd_error"] = str(exc)
    return result


def percentile(values, percent):
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100.0
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def stats_ms(values_ns):
    values = [value / 1e6 for value in values_ns]
    return {
        "count": len(values),
        "mean_ms": statistics.fmean(values),
        "p50_ms": percentile(values, 50),
        "p95_ms": percentile(values, 95),
        "max_ms": max(values),
    }


def verify_size(frame, args, *, image_mode):
    height, width = frame.shape[:2]
    if (width, height) != (args.width, args.height) and not (image_mode and args.allow_other_size):
        raise RuntimeError(
            f"Actual frame is {width}x{height}; requested {args.width}x{args.height}. "
            "Refusing to label a different resolution as the requested benchmark."
        )
    return f"{width}x{height}"


def measure_detector(frame, detector, cv2):
    start = time.perf_counter_ns()
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    gray_end = time.perf_counter_ns()
    corners, ids, rejected = detector.detectMarkers(gray)
    detect_end = time.perf_counter_ns()
    return {
        "grayscale_ns": gray_end - start,
        "detect_ns": detect_end - gray_end,
        "combined_ns": detect_end - start,
        "detected_count": 0 if ids is None else len(ids),
        "detected_ids": [] if ids is None else [int(value) for value in ids.ravel()],
        "rejected_candidates": len(rejected),
    }


def run_images(args, detector, cv2):
    if not args.images.is_dir():
        raise RuntimeError(f"Image folder does not exist: {args.images}")
    paths = sorted(path for path in args.images.iterdir() if path.is_file() and path.suffix.lower() in (".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff"))[:args.max_images]
    if not paths:
        raise RuntimeError(f"No supported image files in {args.images}")
    frames = []
    preload_start = time.perf_counter_ns()
    sizes = Counter()
    for path in paths:
        # imdecode supports Unicode Windows paths unlike some imread builds.
        import numpy as np
        frame = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_COLOR)
        if frame is None:
            raise RuntimeError(f"Could not decode image: {path}")
        sizes[verify_size(frame, args, image_mode=True)] += 1
        frames.append(frame)
    preload_ns = time.perf_counter_ns() - preload_start
    for index in range(args.warmup):
        measure_detector(frames[index % len(frames)], detector, cv2)
    before = platform_state()
    rows = []
    started = time.perf_counter_ns()
    for repeat in range(args.repeats):
        for index, frame in enumerate(frames):
            row = measure_detector(frame, detector, cv2)
            row.update(image=paths[index].name, repeat=repeat + 1, size=f"{frame.shape[1]}x{frame.shape[0]}")
            rows.append(row)
    elapsed_ns = time.perf_counter_ns() - started
    return rows, elapsed_ns, {
        "mode": "preloaded_image_compute_only",
        "image_folder": str(args.images.resolve()),
        "unique_images": len(frames),
        "input_dimensions_and_counts": dict(sizes),
        "repeats": args.repeats,
        "preload_decode_excluded_ms": preload_ns / 1e6,
        "preloaded_bgr_bytes": sum(frame.nbytes for frame in frames),
        "state_before": before,
        "state_after": platform_state(),
        "excludes": ["file_read", "JPEG_or_other_image_decode", "USB_camera", "pose_estimation", "tracking_filter", "GUI", "BLE"],
    }


def run_camera(args, detector, cv2):
    if args.camera_index < 0:
        raise RuntimeError("Camera index must not be negative")
    backend_name = args.backend
    if backend_name == "auto":
        backend_name = {"Windows": "dshow", "Linux": "v4l2"}.get(platform.system(), "any")
    backend = {"dshow": cv2.CAP_DSHOW, "v4l2": cv2.CAP_V4L2, "any": cv2.CAP_ANY}[backend_name]
    camera = cv2.VideoCapture(args.camera_index, backend)
    try:
        if not camera.isOpened():
            raise RuntimeError(f"Could not open camera {args.camera_index} using {backend_name}")
        requested = {
            "fourcc": camera.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*args.fourcc)),
            "width": camera.set(cv2.CAP_PROP_FRAME_WIDTH, args.width),
            "height": camera.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height),
            "fps": camera.set(cv2.CAP_PROP_FPS, args.fps),
        }
        # Validation frame is excluded from measurements even when warmup=0.
        for _ in range(max(1, args.warmup)):
            ok, frame = camera.read()
            if not ok or frame is None:
                raise RuntimeError("Camera did not return a warmup/validation frame")
            verify_size(frame, args, image_mode=False)
            measure_detector(frame, detector, cv2)
        before = platform_state()
        rows = []
        started = time.perf_counter_ns()
        while (time.perf_counter_ns() - started) / 1e9 < args.seconds:
            read_start = time.perf_counter_ns()
            ok, frame = camera.read()
            read_end = time.perf_counter_ns()
            if not ok or frame is None:
                raise RuntimeError("Camera read failed during benchmark; no complete-run result produced")
            size = verify_size(frame, args, image_mode=False)
            row = measure_detector(frame, detector, cv2)
            row.update(read_decode_ns=read_end - read_start, read_and_compute_ns=time.perf_counter_ns() - read_start, size=size)
            rows.append(row)
        elapsed_ns = time.perf_counter_ns() - started
        return rows, elapsed_ns, {
            "mode": "live_camera_sequential_read_and_detect",
            "camera_index": args.camera_index,
            "backend": camera.getBackendName(),
            "requested_backend": backend_name,
            "requested_size": [args.width, args.height],
            "requested_fps": args.fps,
            "requested_fourcc": args.fourcc,
            "property_set_results": requested,
            "driver_reported_fps_not_measured": camera.get(cv2.CAP_PROP_FPS),
            "state_before": before,
            "state_after": platform_state(),
            "excludes": ["pose_estimation", "tracking_filter", "GUI", "BLE"],
            "timing_limitation": "Sequential read/decode + detection; not sensor exposure timing, camera latency, or producer-thread tracking FPS.",
        }
    finally:
        camera.release()


def main(argv=None):
    args = parser().parse_args(argv)
    if args.warmup < 0:
        parser().error("--warmup must be zero or positive")
    if len(args.fourcc) != 4 or not args.fourcc.isascii():
        parser().error("--fourcc must contain exactly four ASCII characters")
    import cv2
    from track_aruco_4k import make_detector
    if args.threads is not None:
        cv2.setNumThreads(args.threads)
    detector = make_detector()
    rows, elapsed_ns, details = run_images(args, detector, cv2) if args.images is not None else run_camera(args, detector, cv2)
    if not rows:
        raise RuntimeError("No measured frames")
    measured = {name.removesuffix("_ns"): stats_ms([row[name] for row in rows]) for name in ("grayscale_ns", "detect_ns", "combined_ns")}
    if args.camera_index is not None:
        measured.update({name.removesuffix("_ns"): stats_ms([row[name] for row in rows]) for name in ("read_decode_ns", "read_and_compute_ns")})
    payload = {
        "benchmark_version": 1,
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python_version": platform.python_version(),
        "opencv_version": cv2.__version__,
        "opencv_threads": cv2.getNumThreads(),
        "requested_threads": args.threads,
        "logical_cpu_count": os.cpu_count(),
        "opencv_cpu_features": cv2.getCPUFeaturesLine() if hasattr(cv2, "getCPUFeaturesLine") else None,
        "opencv_build_information": cv2.getBuildInformation(),
        "detector_factory": "track_aruco_4k.make_detector",
        "dictionary": "DICT_4X4_50",
        "warmup_frames_requested": args.warmup,
        "measured_frames": len(rows),
        "measured_frame_dimensions_and_counts": dict(Counter(row["size"] for row in rows)),
        "frames_with_any_detection": sum(row["detected_count"] > 0 for row in rows),
        "total_marker_detections": sum(row["detected_count"] for row in rows),
        "unique_detected_ids": sorted({marker for row in rows for marker in row["detected_ids"]}),
        "elapsed_seconds": elapsed_ns / 1e9,
        "measured_loop_fps": len(rows) * 1e9 / elapsed_ns,
        "compute_only_throughput_fps": len(rows) * 1e9 / sum(row["combined_ns"] for row in rows),
        "stage_timings": measured,
        "run": details,
        "frames": rows,
    }
    encoded = json.dumps(payload, ensure_ascii=False, indent=2, allow_nan=False)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
        print(f"JSON: {args.output.resolve()}", file=sys.stderr)
    else:
        print(encoded)
    combined = measured["combined"]
    print(f"{details['mode']}: {len(rows)} frames, loop {payload['measured_loop_fps']:.2f} FPS; gray+detect p50 {combined['p50_ms']:.2f} ms / p95 {combined['p95_ms']:.2f} ms / max {combined['max_ms']:.2f} ms", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ImportError, OSError, RuntimeError) as exc:
        print(f"Benchmark failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
