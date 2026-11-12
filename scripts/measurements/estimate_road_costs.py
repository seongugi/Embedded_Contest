#!/usr/bin/env python3
"""Reproduce geometry-mapped cost estimates from published measurement inputs.

Default: read metrics.json, clip_sizes.csv and scenarios.csv, then regenerate the
two cost result files. --check compares regenerated results without writing.
--source-root RUN_DIRECTORY additionally reconstructs the 100-row clip extract
from archived analysis/events_merged.csv and craft4/events_journal.jsonl.
"""

import argparse
import csv
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import re
import sys


EPOCH_MS = 1072915200000
CAPTURE_LEGACY_OFFSET_MS = -5000
CLIP_FIELDS = ["experiment_index", "event_id", "job_id", "bytes", "metadata_bytes", "spool_bytes",
               "source_line", "metadata_source_line", "spool_source_line"]
COUNT_FIELDS = ["N", "N1", "N2", "visible_independent", "visible_but_first_rejected"]
KST = timezone(timedelta(hours=9))


def canonical(raw):
    raw.decode("utf-8")
    return raw.replace(b"\r\n", b"\n")


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


def csv_bytes(rows, fields):
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return stream.getvalue().encode("utf-8")


def read_csv(raw):
    return list(csv.DictReader(io.StringIO(raw.decode("utf-8-sig"), newline="")))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def extract_clips(source_root, published_events, expected_count):
    events_path = source_root / "analysis/events_merged.csv"
    journal_path = source_root / "craft4/events_journal.jsonl"
    events_raw = events_path.read_bytes()
    require(canonical(events_raw) == canonical(published_events),
            "archived events_merged.csv differs from the published measurement records")
    events = read_csv(events_raw)
    triggers = [row for row in events if row["node"] == "craft2"
                and row["stage"] == "ACCIDENT_TRIGGER" and row["matched"] == "True"]
    require(len(triggers) == expected_count, "unexpected number of matched accident triggers")
    stored = {row["canonical_event_id"] for row in events if row["node"] == "craft4"
              and row["stage"] == "STORAGE_CONFIRMED" and row["matched"] == "True"}
    expected = {}
    for row in triggers:
        action_id = (int(row["station"]) << 16) | int(row["seq"])
        unix_ns = (int(row["its"]) + EPOCH_MS + CAPTURE_LEGACY_OFFSET_MS) * 1000000
        event_id = row["canonical_event_id"]
        match = re.fullmatch(r"EVT_(\d{8})_(\d{9})_CAR02", event_id)
        require(match is not None, f"unexpected canonical event ID: {event_id}")
        event_time = datetime.strptime("".join(match.groups()), "%Y%m%d%H%M%S%f").replace(tzinfo=KST)
        delta = event_time - datetime(1970, 1, 1, tzinfo=timezone.utc)
        id_unix_ns = (delta.days * 86400 + delta.seconds) * 1000000000 + delta.microseconds * 1000
        require(unix_ns == id_unix_ns, f"legacy ITS conversion disagrees with {event_id}")
        job_id = f"{action_id}-{unix_ns}"
        require(job_id not in expected and event_id in stored, "duplicate or unstored experiment event")
        expected[job_id] = row
    require(len(expected) == expected_count and len(stored) == expected_count,
            "expected one unique stored craft4 clip per experiment")

    pattern = re.compile(r"\bjob_id=(\d+-\d+) bytes=(\d+) already_present=([01])\b")
    matched = {}
    camera_sizes, metadata_sizes = {}, {}
    counts = {"spool_commit_records": 0, "already_present_skipped": 0, "unrelated_new_jobs_skipped": 0}
    journal_hash = hashlib.sha256()
    with journal_path.open("rb") as handle:
        for line_number, line in enumerate(handle, 1):
            journal_hash.update(line)
            record = json.loads(line)
            message = record.get("MESSAGE", "")
            if record.get("_HOSTNAME") != "craft4" or record.get("_SYSTEMD_UNIT") != "uploader.service":
                continue
            for event, field, sizes in (("CAMERA_READY_ACCEPTED", "video_bytes", camera_sizes),
                                         ("UPLOAD_START", "metadata_bytes", metadata_sizes)):
                if f"event={event} " in message:
                    size_match = re.search(r"\bjob_id=(\d+-\d+).*?\b" + field + r"=(\d+)\b", message)
                    require(size_match is not None, f"unparsed {event} at line {line_number}")
                    job_id, size = size_match.groups()
                    if job_id in expected:
                        require(job_id not in sizes, f"duplicate {event} for {job_id}")
                        sizes[job_id] = (int(size), line_number)
            if "event=SPOOL_COMMIT_COMPLETE " not in message:
                continue
            counts["spool_commit_records"] += 1
            match = pattern.search(message)
            require(match is not None, f"unparsed SPOOL_COMMIT_COMPLETE at line {line_number}")
            job_id, size, already_present = match.groups()
            if already_present == "1":
                counts["already_present_skipped"] += 1
                continue
            if job_id not in expected:
                counts["unrelated_new_jobs_skipped"] += 1
                continue
            require(job_id not in matched, f"duplicate new clip commit: {job_id}")
            require(int(size) > 0, f"nonpositive clip size: {job_id}")
            source = expected[job_id]
            matched[job_id] = {
                "experiment_index": int(source["experiment_index"]),
                "event_id": source["canonical_event_id"], "job_id": job_id,
                "spool_bytes": int(size), "spool_source_line": line_number,
            }
    require(set(matched) == set(expected), "some experiment clips have no unique new spool commit")
    require(set(camera_sizes) == set(expected) and set(metadata_sizes) == set(expected),
            "some experiment clips lack separate video and metadata size records")
    for job_id, row in matched.items():
        row["bytes"], row["source_line"] = camera_sizes[job_id]
        row["metadata_bytes"], row["metadata_source_line"] = metadata_sizes[job_id]
        require(row["bytes"] > 0 and row["metadata_bytes"] > 0
                and row["bytes"] + row["metadata_bytes"] == row["spool_bytes"],
                f"video plus metadata size does not equal the committed spool size: {job_id}")
    rows = sorted(matched.values(), key=lambda row: row["experiment_index"])
    require([row["experiment_index"] for row in rows] == list(range(1, expected_count + 1)),
            "experiment indices are not the complete expected sequence")
    evidence = {
        "archive_run_id": "20260928_e2e_04",
        "hash_basis": "Unmodified original file bytes; all source_line columns are one-based in the JSONL journal.",
        "source_sha256": {"analysis/events_merged.csv": sha(events_raw),
                          "craft4/events_journal.jsonl": journal_hash.hexdigest()},
        "join": {
            "trigger": "matched craft2 ACCIDENT_TRIGGER; matching craft4 STORAGE_CONFIRMED required",
            "action_id": "(station << 16) | seq",
            "job_unix_ns": "(its + 1072915200000 - 5000) * 1000000",
            "legacy_capture_offset_ms": CAPTURE_LEGACY_OFFSET_MS,
            "time_note": "This capture's mock producer added 5000 ms to ITS time; the captured job IDs use its inverse. This is a legacy capture mapping, not a general epoch rule or new calibration.",
            "cross_check": "canonical_event_id local timestamp (UTC+09:00) must equal the derived Unix nanoseconds",
            "journal": "Exact full job_id across craft4 uploader.service CAMERA_READY_ACCEPTED (video_bytes), UPLOAD_START (metadata_bytes), and SPOOL_COMMIT_COMPLETE (bytes, already_present=0). Video plus metadata must equal spool bytes for every clip.",
        },
        "matched_unique_clips": len(rows), **counts,
        "byte_meaning": "clip_sizes.bytes is MP4-only video_bytes from CAMERA_READY_ACCEPTED (source_line). metadata_bytes and spool_bytes retain the separate metadata and combined sizes with their own source lines. No MP4 files are copied.",
    }
    return rows, evidence


def build_inputs(metrics_raw, scenarios_raw, clips_raw, evidence):
    return {
        "schema_version": 1,
        "model": "geometry_mapped_historical_unit_cost_estimate",
        "cycle_seconds": 15,
        "decimal_bytes_per_MB": 1000000,
        "published_input_sha256": {"../metrics.json": sha(canonical(metrics_raw)),
                                   "scenarios.csv": sha(canonical(scenarios_raw)),
                                   "clip_sizes.csv": sha(canonical(clips_raw))},
        "published_hash_basis": "UTF-8 bytes with CRLF converted to LF; original BOM presence preserved.",
        "clip_evidence": evidence,
        "cpu_metric_groups": {"fov_reject": "g1", "ai_not_found": "g2", "ai_found_and_upload": "g3"},
        "formulas": {
            "clip_MB": "sum(clip_sizes.bytes) / clip_count / 1000000",
            "branch_core_seconds": "metrics.groups[group].stats.cpu_cores.delta_from_idle * 15",
            "network_all_upload_MB": "N * clip_MB",
            "network_fov_only_MB": "N1 * clip_MB",
            "network_two_stage_MB": "N2 * clip_MB",
            "cpu_all_ai_core_seconds": "visible_independent * positive_cost + (N - visible_independent) * negative_cost",
            "cpu_two_stage_core_seconds": "(N - N1) * fov_cost + (N1 - N2) * negative_cost + N2 * positive_cost",
            "reduction_fraction": "1 - estimated_cost / matching_baseline_cost",
            "road_and_total_reductions": "ratio of summed costs, not the unweighted mean of scenario percentages",
        },
        "scope": [
            "Historical 2026-09-28 hardware costs use the pre-event 10-second video implementation and 15-second device-wide averages, not the later pre-8/post-2 implementation.",
            "The same observed unit costs are applied to synthetic random road scenes. Geometry maps cost branches; it is not an AI accuracy assumption or a new performance measurement.",
            "Network estimates count MP4 payload bytes only, excluding the separately recorded metadata, protocol overhead, retransmissions and other device traffic. Equal mean clip size is assumed for every modeled upload.",
            "CPU is idle-subtracted device-wide core-seconds, including branch processing and encoding/upload where present. It is not isolated model inference time.",
            "The CPU comparator runs AI for all neighbors, mapping independent geometry visibility to positive or negative measured branch costs. No CPU claim is made for the all-upload network comparator.",
            "The accident vehicle's own video and CPU are a common term outside all reported witness totals; no own-video size is assumed.",
            "Retained and missed independently visible vehicle counts are reported alongside the estimated cost reductions.",
        ],
    }


def calculate(counts, units):
    n, n1, n2, visible, missed = (counts[key] for key in COUNT_FIELDS)
    require(0 <= n2 <= n1 <= n and n > 0 and 0 <= visible <= n
            and visible - missed == n2 and missed >= 0, "inconsistent geometry populations")
    clip = units["mean_clip_MB"]
    costs = units["cpu_branch_core_seconds"]
    all_ai = visible * costs["ai_found_and_upload"] + (n - visible) * costs["ai_not_found"]
    two_stage = ((n - n1) * costs["fov_reject"] + (n1 - n2) * costs["ai_not_found"]
                 + n2 * costs["ai_found_and_upload"])
    return {
        **counts,
        "network_all_upload_MB": n * clip,
        "network_fov_only_MB": n1 * clip,
        "network_two_stage_MB": n2 * clip,
        "network_fov_reduction_fraction": 1 - n1 / n,
        "network_second_conditional_reduction_fraction": 1 - n2 / n1 if n1 else None,
        "network_total_reduction_fraction": 1 - n2 / n,
        "cpu_all_ai_core_seconds": all_ai,
        "cpu_two_stage_core_seconds": two_stage,
        "cpu_reduction_fraction": 1 - two_stage / all_ai if all_ai else None,
        "independent_visible_retention_fraction": n2 / visible if visible else None,
    }


def estimate(inputs, metrics, scenario_rows, clip_rows):
    require(inputs["schema_version"] == 1 and inputs["cycle_seconds"] == 15
            and inputs["decimal_bytes_per_MB"] == 1000000, "unsupported cost input configuration")
    expected_count = metrics["conditions"]["planned_mock_events"]
    require(len(clip_rows) == expected_count == inputs["clip_evidence"]["matched_unique_clips"],
            "unexpected clip sample count")
    require(len({row["job_id"] for row in clip_rows}) == expected_count
            and len({row["event_id"] for row in clip_rows}) == expected_count,
            "duplicate clip identities")
    sizes = [int(row["bytes"]) for row in clip_rows]
    require(all(size > 0 for size in sizes), "nonpositive clip size")
    require(all(int(row["bytes"]) + int(row["metadata_bytes"]) == int(row["spool_bytes"])
                for row in clip_rows), "video/metadata/spool sizes do not reconcile")
    costs, cpu_source = {}, {}
    for branch, group in inputs["cpu_metric_groups"].items():
        cpu = metrics["groups"][group]["stats"]["cpu_cores"]
        require(math.isclose(cpu["avg"] - cpu["idle_avg"], cpu["delta_from_idle"], abs_tol=1e-12),
                f"idle subtraction mismatch for {group}")
        costs[branch] = cpu["delta_from_idle"] * inputs["cycle_seconds"]
        cpu_source[branch] = {"metrics_group": group, "average_cores": cpu["avg"],
                              "idle_cores": cpu["idle_avg"], "excess_cores": cpu["delta_from_idle"]}
    units = {"clip_count": len(sizes), "total_observed_clip_bytes": sum(sizes),
             "mean_clip_bytes": sum(sizes) / len(sizes),
             "mean_clip_MB": sum(sizes) / len(sizes) / inputs["decimal_bytes_per_MB"],
             "total_observed_metadata_bytes": sum(int(row["metadata_bytes"]) for row in clip_rows),
             "total_observed_spool_bytes": sum(int(row["spool_bytes"]) for row in clip_rows),
             "min_clip_bytes": min(sizes), "max_clip_bytes": max(sizes),
             "cpu_branch_core_seconds": costs, "cpu_source_values": cpu_source}
    require(len({row["scenario_id"] for row in scenario_rows}) == len(scenario_rows)
            and len(scenario_rows) > 0, "duplicate or missing scenarios")
    estimates = [{"scenario_id": row["scenario_id"], "road": row["road"],
                  **calculate({key: int(row[key]) for key in COUNT_FIELDS}, units)} for row in scenario_rows]

    def aggregate(rows):
        counts = {key: sum(row[key] for row in rows) for key in COUNT_FIELDS}
        total = calculate(counts, units)
        additive = COUNT_FIELDS + [key for key in total if key.endswith("_MB") or key.endswith("_core_seconds")]
        return {"scenario_count": len(rows), "totals": total,
                "mean_per_scenario": {key: total[key] / len(rows) for key in additive}}

    summary = {"schema_version": 1, "model": inputs["model"],
               "input_manifest": "cost_inputs.json", "historical_measurement_run": metrics["measurement_version"],
               "cycle_seconds": inputs["cycle_seconds"], "unit_costs": units,
               **aggregate(estimates),
               "by_road": {road: aggregate([row for row in estimates if row["road"] == road])
                           for road in sorted({row["road"] for row in estimates})},
               "scope": inputs["scope"]}
    return estimates, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path,
                        default=Path(__file__).resolve().parents[2] / "docs/measurements/road-simulation")
    parser.add_argument("--source-root", type=Path, help="archived run directory containing analysis/ and craft4/")
    parser.add_argument("--check", action="store_true", help="verify reproduced artifacts without writing files")
    args = parser.parse_args()
    directory = args.directory.resolve()
    try:
        metrics_raw = (directory.parent / "metrics.json").read_bytes()
        metrics = json.loads(metrics_raw)
        scenarios_raw = (directory / "scenarios.csv").read_bytes()
        generated = {}
        if args.source_root is not None:
            clips, evidence = extract_clips(args.source_root,
                                           (directory.parent / "20260928-e2e/events_merged.csv").read_bytes(),
                                           metrics["conditions"]["planned_mock_events"])
            clips_raw = csv_bytes(clips, CLIP_FIELDS)
            inputs = build_inputs(metrics_raw, scenarios_raw, clips_raw, evidence)
            generated.update({"clip_sizes.csv": clips_raw, "cost_inputs.json": json_bytes(inputs)})
        else:
            clips_raw = (directory / "clip_sizes.csv").read_bytes()
            inputs = json.loads((directory / "cost_inputs.json").read_bytes())
            clips = read_csv(clips_raw)
        for name, raw in (("../metrics.json", metrics_raw), ("scenarios.csv", scenarios_raw), ("clip_sizes.csv", clips_raw)):
            require(sha(canonical(raw)) == inputs["published_input_sha256"][name],
                    f"published input hash mismatch: {name}")
        estimates, summary = estimate(inputs, metrics, read_csv(scenarios_raw), clips)
        generated.update({"cost_estimates.csv": csv_bytes(estimates, list(estimates[0])),
                          "cost_summary.json": json_bytes(summary)})
        for name, raw in generated.items():
            path = directory / name
            if args.check:
                require(canonical(path.read_bytes()) == raw, f"reproduced output differs: {name}")
            else:
                path.write_bytes(raw)
        print(f"PASS: {len(clips)} unique measured clips; {len(estimates)} scenarios; "
              f"{len(generated)} files {'verified' if args.check else 'written'}.")
        print(f"Measured mean payload: {summary['unit_costs']['mean_clip_bytes']:.2f} bytes.")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
