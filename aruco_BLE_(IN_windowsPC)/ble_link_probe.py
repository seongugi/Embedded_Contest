"""Send synthetic poses ONLY to an explicitly named ACPTEST1..9 test device.

This program never reads a camera or calls vehicle IPC. It is a temporary BLE
link check, and must not be mistaken for the measured-position tracking entry.
"""
import argparse
import json
import math
from pathlib import Path
import time

from aruco_ble_sender import BLEPoseSender


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="ACPTEST1")
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--connect-seconds", type=float, default=25)
    parser.add_argument("--output", default="pc_probe_result.json")
    args = parser.parse_args()
    if not args.name.startswith("ACPTEST") or len(args.name) != 8 or args.name[-1] not in "123456789":
        parser.error("Synthetic data may be sent only to an ACPTEST1..9 test name")
    if not math.isfinite(args.seconds) or not 10 <= args.seconds <= 300:
        parser.error("--seconds must be between 10 and 300")
    if not math.isfinite(args.connect_seconds) or not 1 <= args.connect_seconds <= 120:
        parser.error("--connect-seconds must be between 1 and 120")
    settings = {"cars": [{"marker_id": 1, "name": args.name}],
                "scan_timeout_s": 3, "reconnect_delay_s": 1,
                "max_rate_hz": 30, "max_sample_age_ms": 300}
    sender = BLEPoseSender(settings)
    report = {"purpose": "SYNTHETIC BLE LINK PROBE; NOT CAMERA/GPS DATA",
              "target": args.name, "requested_seconds": args.seconds,
              "generated_pose_frames": 0, "generated_lost_frames": 0,
              "wire_protocol": "ACP1 40 bytes / ACT1 20-byte clock read",
              "phases": ["pose(x123,y456,heading12.5)", "flags=0 fresh frames for 2 seconds",
                         "pose(x988,y654,heading315)"]}
    code = 0
    try:
        print("SYNTHETIC PROBE ONLY: no camera, IPC or vehicle control", flush=True)
        sender.start()
        connect_deadline = time.monotonic() + args.connect_seconds
        while sender.snapshot()["cars"][1]["state"] != "ready":
            if sender.snapshot()["fatal_error"]:
                raise RuntimeError(sender.snapshot()["fatal_error"])
            if time.monotonic() >= connect_deadline:
                raise TimeoutError("Temporary probe did not connect and synchronize")
            time.sleep(.05)
        print("Connected and clock synchronized; beginning synthetic frames", flush=True)
        started = time.monotonic()
        next_frame = started
        frame = 0
        lost_start = min(7.0, args.seconds / 3)
        while time.monotonic() - started < args.seconds:
            elapsed = time.monotonic() - started
            frame += 1
            if lost_start <= elapsed < lost_start + 2:
                vehicles = {}
                report["generated_lost_frames"] += 1
            else:
                pose = (123.25, 456.5, 12.5) if elapsed < lost_start else (987.5, 654.25, -45.0)
                vehicles = {1: dict(zip(("x_mm", "y_mm", "heading_deg"), pose))}
                report["generated_pose_frames"] += 1
            sender.publish_frame(frame, time.monotonic_ns(), vehicles)
            next_frame += 1 / 30
            # Do not catch up by enqueueing missed synthetic frames.
            now = time.monotonic()
            if next_frame < now:
                next_frame = now
            time.sleep(max(0, next_frame - now))
        report["elapsed_seconds"] = time.monotonic() - started
        sender.invalidate_all("sender_stopping")
        time.sleep(.1)
        report["snapshot_before_stop"] = sender.snapshot()
    except (Exception, KeyboardInterrupt) as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        report["snapshot_at_error"] = sender.snapshot()
        code = 1
    finally:
        sender.stop()
        report["snapshot_after_stop"] = sender.snapshot()
        Path(args.output).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(report, indent=2), flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
