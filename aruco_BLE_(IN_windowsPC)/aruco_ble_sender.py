"""Windows BLE central for five ArUco vehicles; no prediction or vehicle control.

publish_frame() only replaces a bounded latest-value slot. BLE operations run in
one background asyncio thread with an independent task for each vehicle. Bleak
is imported only when a real backend starts; offline tests need no Bluetooth.
"""
from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass, field
import json
import math
import os
from pathlib import Path
import re
import sys
import threading
import time
from typing import Callable
import uuid

from aruco_ble_protocol import (
    calculate_clock_sample, decode_clock, pack_invalid, pack_pose,
)

SERVICE_UUID = "7b9c0001-6a54-4f1b-9f65-2c7b8d1e4a90"
POSE_UUID = "7b9c0002-6a54-4f1b-9f65-2c7b8d1e4a90"
CLOCK_UUID = "7b9c0003-6a54-4f1b-9f65-2c7b8d1e4a90"
REASONS = {"not_detected": 1, "camera_unavailable": 2, "sample_too_old": 3,
           "reference_unavailable": 4, "invalid_measurement": 5,
           "sender_stopped": 6, "awaiting_observation": 7,
           "no_reference": 4, "sender_stopping": 6}


def load_config(source: str | Path | dict) -> dict:
    """Read/validate a BLE section or a JSON containing a 'ble_sender' section."""
    if isinstance(source, (str, Path)):
        with Path(source).open(encoding="utf-8-sig") as handle:
            source = json.load(handle)
    if not isinstance(source, dict):
        raise ValueError("BLE configuration must be an object")
    raw = source.get("ble_sender", source)
    if not isinstance(raw, dict):
        raise ValueError("ble_sender must be an object")
    cfg = dict(raw)
    if cfg.get("enabled", True) is not True:
        raise ValueError("BLE sender is disabled in configuration")
    if "rx_uuid" in cfg or "tx_uuid" in cfg:
        raise ValueError("Legacy BLE configuration is incompatible; use ACP1 pose_uuid and ACT1 clock_uuid")
    for key, default in (("service_uuid", SERVICE_UUID), ("pose_uuid", POSE_UUID),
                         ("clock_uuid", CLOCK_UUID)):
        try:
            cfg[key] = str(uuid.UUID(str(cfg.get(key, default))))
        except (ValueError, TypeError, AttributeError) as exc:
            raise ValueError(f"{key} must be a valid UUID") from exc
    if len({cfg[key] for key in ("service_uuid", "pose_uuid", "clock_uuid")}) != 3:
        raise ValueError("Service, POSE and CLOCK UUIDs must be different")
    defaults = {"scan_timeout_s": 4.0, "reconnect_delay_s": 2.0,
                "connect_timeout_s": 15.0, "write_timeout_s": 2.0,
                "sync_timeout_s": 2.0, "sync_interval_s": 10.0,
                "max_clock_rtt_ms": 100.0,
                "max_sample_age_ms": 300.0, "max_rate_hz": 30.0}
    for key, default in defaults.items():
        value = cfg.get(key, default)
        if isinstance(value, bool):
            raise ValueError(f"{key} must be a positive number")
        try:
            value = float(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{key} must be a positive number") from exc
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"{key} must be a positive number")
        cfg[key] = value
    if cfg["max_rate_hz"] > 120:
        raise ValueError("max_rate_hz must be at most 120")
    if cfg["max_sample_age_ms"] > 300:
        raise ValueError("ACP1 samples must be younger than 300 ms; max_sample_age_ms cannot exceed 300")
    if cfg["max_clock_rtt_ms"] > 100:
        raise ValueError("ACT1 clock read RTT limit cannot exceed 100 ms")
    samples = cfg.get("initial_clock_samples", 5)
    if isinstance(samples, bool) or not isinstance(samples, int) or not 2 <= samples <= 20:
        raise ValueError("initial_clock_samples must be an integer from 2 to 20")
    cfg["initial_clock_samples"] = samples
    cars = cfg.get("cars")
    if not isinstance(cars, list) or not cars:
        raise ValueError("Configure cars with marker_id and an exact BLE address or name")
    ids, vehicle_ids, addresses, names, active = set(), set(), set(), set(), []
    for entry in cars:
        if not isinstance(entry, dict):
            raise ValueError("Each car must be an object")
        car = dict(entry)
        marker_id = car.get("marker_id")
        if isinstance(marker_id, bool) or not isinstance(marker_id, int) or not 1 <= marker_id <= 5:
            raise ValueError("Vehicle marker_id must be an integer from 1 to 5")
        if marker_id in ids:
            raise ValueError(f"Duplicate marker_id: {marker_id}")
        ids.add(marker_id)
        if not isinstance(car.get("enabled", True), bool):
            raise ValueError("Car enabled must be true or false")
        if car.get("enabled", True) is False:
            continue
        vehicle_id = car.get("vehicle_id", marker_id)
        if isinstance(vehicle_id, bool) or not isinstance(vehicle_id, int) or not 1 <= vehicle_id <= 65535:
            raise ValueError("vehicle_id must be an integer from 1 to 65535")
        if vehicle_id in vehicle_ids:
            raise ValueError(f"Duplicate vehicle_id: {vehicle_id}")
        vehicle_ids.add(vehicle_id)
        car["vehicle_id"] = vehicle_id
        address = car.get("address") or ""
        name = car.get("name") or ""
        if not isinstance(address, str) or not isinstance(name, str):
            raise ValueError("BLE address and name must be strings")
        address, name = address.strip().upper(), name.strip()
        if bool(address) == bool(name):
            raise ValueError(f"ID{marker_id:02d}: set exactly one of address or name")
        if address and not re.fullmatch(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", address):
            raise ValueError(f"ID{marker_id:02d}: address must have format AA:BB:CC:DD:EE:FF")
        if address in addresses or name in names:
            raise ValueError("Two vehicles cannot use the same BLE address or name")
        if address:
            addresses.add(address)
        if name:
            names.add(name)
        car.update(address=address, name=name)
        active.append(car)
    if not active:
        raise ValueError("No enabled BLE vehicles; configure at least one target before starting")
    cfg["cars"] = active
    return cfg


class BleakBackend:
    """Small injection boundary: discover(timeout), make_client(device, callback)."""
    def __init__(self):
        vendor = Path(__file__).with_name("vendor")
        if vendor.is_dir() and str(vendor) not in sys.path:
            sys.path.insert(0, str(vendor))
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError as exc:
            raise RuntimeError("BLE support is missing. Install requirements_ble.txt in the tracking Python environment.") from exc
        self.client_type, self.scanner_type = BleakClient, BleakScanner

    async def discover(self, timeout: float):
        # One caller per sender. Returning BLEDevice avoids each connect() doing
        # its own implicit scan (a known problem for concurrent Bleak clients).
        found = await self.scanner_type.discover(timeout=timeout, return_adv=True)
        return [(device, advertisement.local_name or device.name or "")
                for device, advertisement in found.values()]

    def make_client(self, device, disconnected_callback):
        options = {"winrt": {"use_cached_services": False}} if sys.platform == "win32" else {}
        return self.client_type(device, disconnected_callback=disconnected_callback, **options)


@dataclass(frozen=True)
class Observation:
    frame_seq: int
    capture_ns: int
    pose: tuple[float, float, float] | None
    reason: int = 1


@dataclass
class _Link:
    car: dict
    latest: Observation | None = None
    accepting: bool = False
    epoch: int | None = None
    sequence: int = 0
    reserved_sequences: int = 0
    last_capture_pi_ns: int | None = None
    status: dict = field(default_factory=lambda: {
        "state": "stopped", "connected": False, "synchronized": False,
        "sent_pose": 0, "sent_lost": 0, "dropped": 0,
        "replaced": 0, "errors": 0, "last_error": "", "last_connection_error": "",
        "last_frame_seq": None, "clock_rtt_ms": None,
        "clock_uncertainty_ms": None,
        "mtu": None, "epoch": None, "last_capture_pi_ns": None,
    })


class BLEPoseSender:
    """Nonblocking tracker-facing transport. All times are PC monotonic ns.

    backend may be a fake with async discover(timeout) -> [(BLEDevice, name)]
    and make_client(device, disconnected_callback) -> Bleak-shaped client.
    No observation from a disconnected/handshaking period is replayed.
    """
    def __init__(self, config: dict | str | Path, *, backend=None,
                 clock_ns: Callable[[], int] = time.monotonic_ns,
                 sequence_state_path: str | Path | None = None):
        self.config = load_config(config)
        self._backend = backend
        self._sequence_file = (Path(sequence_state_path) if sequence_state_path is not None
                               else Path(__file__).with_name("ble_sequence_state.json")
                               if backend is None else None)
        self._sequence_state = {}
        if self._sequence_file is not None and self._sequence_file.exists():
            self._sequence_state = json.loads(self._sequence_file.read_text(encoding="utf-8"))
            if not isinstance(self._sequence_state, dict):
                raise ValueError("Invalid BLE sequence state")
            for value in self._sequence_state.values():
                if (not isinstance(value, dict)
                        or type(value.get("epoch")) is not int or not 0 < value["epoch"] < 2**64
                        or type(value.get("reserved")) is not int or not 0 <= value["reserved"] < 2**32):
                    raise ValueError("Invalid BLE sequence state")
        self._clock_ns = clock_ns
        self._lock = threading.Lock()
        self._links = {car["marker_id"]: _Link(car) for car in self.config["cars"]}
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._started = threading.Event()
        self._loop = None
        self._runner_task = None
        self._fatal_error = ""
        self._last_frame_seq = -1

    def start(self):
        if self._thread is not None and self._thread.is_alive():
            return self
        self._stop.clear()
        self._started.clear()
        self._fatal_error = ""
        with self._lock:
            self._last_frame_seq = -1
        self._thread = threading.Thread(target=self._thread_main,
                                        name="aruco-ble", daemon=True)
        self._thread.start()
        if not self._started.wait(timeout=5.0):
            self.stop()
            raise RuntimeError("BLE background thread did not start")
        if self._fatal_error:
            raise RuntimeError(self._fatal_error)
        return self

    def stop(self, timeout: float = 5.0):
        """Cancel outstanding Bluetooth operations and release connections."""
        self._stop.set()
        loop, task = self._loop, self._runner_task
        if loop is not None and task is not None:
            try:
                loop.call_soon_threadsafe(task.cancel)
            except RuntimeError:
                pass
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout)
            if self._thread.is_alive():
                raise RuntimeError("Bluetooth shutdown did not finish within the timeout")

    def publish_frame(self, frame_seq: int, capture_mono_ns: int,
                      vehicles: dict[int, dict], reason: str = "not_detected"):
        """Publish ONLY the measurements from this frame, including an empty dict.

        Missing IDs generate fresh ACP1 flags=0 frames. frame_seq increases over the
        tracker run; calling twice for the same frame never refreshes a pose.
        """
        if not isinstance(frame_seq, int) or isinstance(frame_seq, bool) or frame_seq < 0:
            raise ValueError("frame_seq must be a nonnegative integer")
        if not isinstance(capture_mono_ns, int) or isinstance(capture_mono_ns, bool) or capture_mono_ns < 0:
            raise ValueError("capture_mono_ns must be nonnegative integer nanoseconds")
        if reason not in REASONS:
            raise ValueError(f"Unknown invalid observation reason: {reason}")
        prepared = {}
        for marker_id in self._links:
            value = vehicles.get(marker_id)
            pose, lost_reason = None, REASONS[reason]
            if value is not None:
                try:
                    pose = tuple(float(value[key]) for key in ("x_mm", "y_mm", "heading_deg"))
                    if not all(math.isfinite(number) for number in pose):
                        raise ValueError("Nonfinite pose")
                except (KeyError, TypeError, ValueError, OverflowError):
                    pose, lost_reason = None, REASONS["invalid_measurement"]
            prepared[marker_id] = Observation(frame_seq, capture_mono_ns, pose, lost_reason)
        with self._lock:
            if frame_seq <= self._last_frame_seq:
                return False
            self._last_frame_seq = frame_seq
            for marker_id, sample in prepared.items():
                link = self._links[marker_id]
                if not link.accepting:
                    if sample.pose is not None:
                        link.status["dropped"] += 1
                    continue
                if link.latest is not None:
                    link.status["replaced"] += 1
                link.latest = sample
        return True

    def invalidate_all(self, reason: str = "camera_unavailable"):
        if reason not in REASONS:
            raise ValueError(f"Unknown invalid observation reason: {reason}")
        with self._lock:
            for link in self._links.values():
                if link.accepting:
                    # No fresh camera frame exists here. Clear pending work;
                    # do not manufacture a current capture timestamp or packet.
                    link.latest = None
                    link.status["observation_state"] = reason

    def snapshot(self) -> dict:
        with self._lock:
            return {"running": bool(self._thread and self._thread.is_alive()),
                    "fatal_error": self._fatal_error,
                    "cars": {marker_id: dict(link.status, pending=int(link.latest is not None))
                             for marker_id, link in self._links.items()}}

    def _update(self, link, **values):
        with self._lock:
            link.status.update(values)

    def _increment(self, link, key, amount=1):
        with self._lock:
            link.status[key] += amount

    def _set_accepting(self, link, accepting):
        with self._lock:
            if link.latest is not None and link.latest.pose is not None:
                link.status["dropped"] += 1
            link.latest = None
            link.accepting = accepting

    def _sequence_key(self, link):
        return f"{link.car['vehicle_id']}:{link.car['address'] or link.car['name']}"

    def _reserve_sequences(self, link):
        if self._sequence_file is None or link.reserved_sequences:
            return
        # Reserve ahead so a process restart never reuses a transmitted sequence.
        # Only epoch/counter metadata is saved; observations stay in the latest slot.
        state = dict(self._sequence_state)
        state[self._sequence_key(link)] = {
            "epoch": link.epoch, "reserved": (link.sequence + 1024) & 0xFFFFFFFF}
        temporary = self._sequence_file.with_name(self._sequence_file.name + ".tmp")
        try:
            with temporary.open("w", encoding="utf-8") as handle:
                json.dump(state, handle)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, self._sequence_file)
        finally:
            temporary.unlink(missing_ok=True)
        self._sequence_state = state
        link.reserved_sequences = 1024

    def _take_latest(self, link, pose_due):
        with self._lock:
            item = link.latest
            if item is None or not pose_due:
                return None
            link.latest = None
            return item

    def _thread_main(self):
        try:
            # This is a dedicated non-GUI thread; do not opt into an STA loop.
            if self._backend is None:
                self._backend = BleakBackend()
            asyncio.run(self._run())
        except Exception as exc:
            self._fatal_error = f"{type(exc).__name__}: {exc}"
        finally:
            self._loop, self._runner_task = None, None
            self._started.set()
            for link in self._links.values():
                self._set_accepting(link, False)
                self._update(link, state="stopped", connected=False, synchronized=False)

    async def _run(self):
        self._loop = asyncio.get_running_loop()
        self._runner_task = asyncio.current_task()
        self._devices = {}
        self._scan_errors = {}
        self._claimed = {}
        tasks = [asyncio.create_task(self._scan_loop(), name="ble-scanner")]
        tasks += [asyncio.create_task(self._car_loop(link), name=f"ble-car-{marker_id}")
                  for marker_id, link in self._links.items()]
        self._started.set()
        try:
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            pass
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    async def _scan_loop(self):
        """Single scanner; do not interrupt healthy links to discover a missing car."""
        while not self._stop.is_set():
            missing = [mid for mid in self._links if mid not in self._devices]
            if not missing:
                await asyncio.sleep(0.1)
                continue
            try:
                found = await self._backend.discover(self.config["scan_timeout_s"])
                for marker_id in missing:
                    car = self._links[marker_id].car
                    matches = {}
                    for device, local_name in found:
                        address = str(device.address).upper()
                        match = address == car["address"] if car["address"] else local_name == car["name"]
                        if match:
                            matches[address] = device
                    if len(matches) > 1:
                        self._scan_errors[marker_id] = "Multiple devices advertise the configured name; set an exact address"
                    elif matches:
                        self._devices[marker_id] = next(iter(matches.values()))
                        self._scan_errors.pop(marker_id, None)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                for marker_id in missing:
                    self._scan_errors[marker_id] = f"Scan failed: {exc}"
            await asyncio.sleep(self.config["reconnect_delay_s"])

    async def _car_loop(self, link):
        marker_id = link.car["marker_id"]
        while not self._stop.is_set():
            self._set_accepting(link, False)
            self._update(link, state="scanning", connected=False, synchronized=False)
            device = self._devices.get(marker_id)
            if device is None:
                self._update(link, last_error=self._scan_errors.get(marker_id)
                             or link.status.get("last_connection_error")
                             or "Waiting for the configured BLE target")
                await asyncio.sleep(0.05)
                continue
            address = str(device.address).upper()
            owner = self._claimed.get(address)
            if owner is not None and owner != marker_id:
                self._update(link, state="configuration_error", last_error="Configured targets resolve to the same BLE device")
                await asyncio.sleep(self.config["reconnect_delay_s"])
                continue
            self._claimed[address] = marker_id
            client = None
            disconnected = asyncio.Event()
            try:
                self._update(link, state="connecting", address=address, last_error="")
                client = self._backend.make_client(device, lambda _client, event=disconnected: event.set())
                await asyncio.wait_for(client.connect(), self.config["connect_timeout_s"])
                if not client.is_connected:
                    raise ConnectionError("BLE connect completed without an active link")
                self._update(link, state="synchronizing", connected=True)
                await self._connected(link, client, disconnected)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self._increment(link, "errors")
                error = f"{type(exc).__name__}: {exc}"
                self._update(link, state="reconnecting", last_error=error,
                             last_connection_error=error)
                self._devices.pop(marker_id, None)
            finally:
                self._set_accepting(link, False)
                self._update(link, connected=False, synchronized=False)
                if client is not None:
                    try:
                        await asyncio.wait_for(client.disconnect(), self.config["write_timeout_s"])
                    except (Exception, asyncio.CancelledError):
                        pass
                if self._claimed.get(address) == marker_id:
                    self._claimed.pop(address, None)
            await asyncio.sleep(self.config["reconnect_delay_s"])

    async def _connected(self, link, client, disconnected):
        vehicle_id = link.car["vehicle_id"]
        async def sync(sample_count):
            accepted = []
            last_error = ""
            latest_epoch = None
            for _ in range(sample_count):
                if disconnected.is_set() or not client.is_connected:
                    raise ConnectionError("Bluetooth link was disconnected")
                t0_ns = self._clock_ns()
                raw = await asyncio.wait_for(
                    client.read_gatt_char(self.config["clock_uuid"], use_cached=False),
                    self.config["sync_timeout_s"])
                t1_ns = self._clock_ns()
                try:
                    clock = decode_clock(bytes(raw))
                    if latest_epoch != clock.epoch:
                        accepted.clear()
                        latest_epoch = clock.epoch
                    accepted.append(calculate_clock_sample(
                        t0_ns, t1_ns, clock, max_rtt_ms=self.config["max_clock_rtt_ms"]))
                except (ValueError, TypeError) as exc:
                    last_error = str(exc)
                await asyncio.sleep(0)
            if not accepted:
                raise RuntimeError(f"No valid ACT1 clock sample (RTT <= {self.config['max_clock_rtt_ms']:g} ms): {last_error}")
            best = min(accepted, key=lambda sample: sample.round_trip_ns)
            if link.epoch != best.epoch:
                link.epoch = best.epoch
                saved = self._sequence_state.get(self._sequence_key(link), {})
                link.sequence = saved.get("reserved", 0) if saved.get("epoch") == best.epoch else 0
                link.reserved_sequences = 0
                link.last_capture_pi_ns = None
            self._update(link, synchronized=True, epoch=best.epoch,
                         clock_rtt_ms=best.round_trip_ns / 1e6,
                         clock_offset_ns=best.offset_ns,
                         clock_uncertainty_ms=None)
            return best

        clock_sample = await sync(self.config["initial_clock_samples"])
        # A fresh ACT1 read completes the link before checking Windows ATT MTU.
        # ACP1 explicitly disallows long/prepared writes. Windows can fragment
        # larger writes automatically, so check the negotiated MTU ourselves.
        mtu = int(client.mtu_size)
        self._update(link, mtu=mtu)
        if mtu < 43:
            raise RuntimeError(f"ACP1 needs ATT MTU >= 43 for one 40-byte Write Request; negotiated {mtu}. No fragmented/prepare write was sent.")

        connected_after_ns = self._clock_ns()
        self._set_accepting(link, True)
        self._update(link, state="ready", last_error="", last_connection_error="")
        next_sync_ns = connected_after_ns + int(self.config["sync_interval_s"] * 1e9)
        last_sent_ns = 0
        period_ns = int(1e9 / self.config["max_rate_hz"])
        max_age_ns = int(self.config["max_sample_age_ms"] * 1e6)
        while not self._stop.is_set():
            if disconnected.is_set() or not client.is_connected:
                raise ConnectionError("Bluetooth link was disconnected")
            now_ns = self._clock_ns()
            if now_ns >= next_sync_ns:
                previous_epoch = link.epoch
                clock_sample = await sync(3)
                if previous_epoch != link.epoch:
                    # A receiver restart invalidates every pending observation.
                    connected_after_ns = self._clock_ns()
                    self._set_accepting(link, True)
                next_sync_ns = self._clock_ns() + int(self.config["sync_interval_s"] * 1e9)
                continue
            item = self._take_latest(link, now_ns - last_sent_ns >= period_ns)
            if item is not None:
                age_ns = now_ns - item.capture_ns
                if item.capture_ns < connected_after_ns or age_ns >= max_age_ns or age_ns < 0:
                    self._increment(link, "dropped")
                    continue
                capture_pi_ns = item.capture_ns + clock_sample.offset_ns
                if capture_pi_ns <= 0 or (link.last_capture_pi_ns is not None and
                                        capture_pi_ns <= link.last_capture_pi_ns):
                    # Offset refresh can move the mapped time backwards. Skip
                    # until a new real frame advances; never clamp to "now".
                    self._increment(link, "dropped")
                    self._update(link, observation_state="timestamp_not_advanced")
                    continue
                self._reserve_sequences(link)
                sequence = (link.sequence + 1) & 0xFFFFFFFF
                try:
                    if item.pose is None:
                        packet = pack_invalid(vehicle_id, sequence, capture_pi_ns, epoch=link.epoch)
                    else:
                        packet = pack_pose(vehicle_id, sequence, capture_pi_ns,
                                           *item.pose, epoch=link.epoch)
                except (ValueError, OverflowError):
                    self._increment(link, "dropped")
                    continue
                if int(client.mtu_size) < 43:
                    raise RuntimeError("ATT MTU dropped below 43; ACP1 packet was not fragmented or sent")
                # This task is the sole writer/clock reader for this link. At
                # most one operation is outstanding; other vehicles are tasks.
                write_started_ns = self._clock_ns()
                # A failed/lost ATT response does not prove the receiver missed
                # the packet. Reserve its sequence and timestamp before the
                # attempt, including across reconnects with the same epoch.
                link.sequence = sequence
                if link.reserved_sequences:
                    link.reserved_sequences -= 1
                link.last_capture_pi_ns = capture_pi_ns
                await asyncio.wait_for(
                    client.write_gatt_char(self.config["pose_uuid"], packet, response=True),
                    self.config["write_timeout_s"])
                last_sent_ns = write_started_ns
                self._increment(link, "sent_pose" if item.pose is not None else "sent_lost")
                self._update(link, last_frame_seq=item.frame_seq,
                             last_capture_pi_ns=capture_pi_ns, sequence=sequence,
                             observation_state="valid" if item.pose is not None else "invalid")
            await asyncio.sleep(0.005)

async def scan_devices(timeout=5.0):
    backend = BleakBackend()
    return [{"address": device.address, "name": name}
            for device, name in await backend.discover(timeout)]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Scan BLE receivers or validate sender configuration")
    parser.add_argument("--scan", action="store_true", help="List nearby BLE names/addresses without connecting")
    parser.add_argument("--scan-seconds", type=float, default=5.0)
    parser.add_argument("--config", default=str(Path(__file__).with_name("ble_config.json")))
    args = parser.parse_args(argv)
    try:
        if args.scan:
            if not math.isfinite(args.scan_seconds) or args.scan_seconds <= 0:
                parser.error("--scan-seconds must be positive")
            print(json.dumps(asyncio.run(scan_devices(args.scan_seconds)), ensure_ascii=False, indent=2))
        else:
            config = load_config(args.config)
            print(f"Configuration valid for {len(config['cars'])} vehicle(s). Start the tracking entry point to send measured poses.")
    except (RuntimeError, ValueError, OSError) as exc:
        parser.exit(1, f"BLE error: {exc}\n")


if __name__ == "__main__":
    main()
