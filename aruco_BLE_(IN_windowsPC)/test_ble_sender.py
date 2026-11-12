"""Offline ACP1/ACT1 tests using simulated GATT, never a radio or vehicle."""
import asyncio
import threading
import time
from types import SimpleNamespace
import unittest
import tempfile
from pathlib import Path

from aruco_ble_protocol import decode_pose, pack_clock
from unittest.mock import Mock, patch

from aruco_ble_sender import BLEPoseSender, BleakBackend, load_config


def config(count=1, **overrides):
    value = {"cars": [{"marker_id": mid, "address": f"00:11:22:33:44:{mid:02X}"}
                      for mid in range(1, count + 1)],
             "scan_timeout_s": .01, "reconnect_delay_s": .02,
             "write_timeout_s": .15, "connect_timeout_s": .2,
             "sync_timeout_s": .1, "sync_interval_s": 10,
             "initial_clock_samples": 3, "max_sample_age_ms": 300,
             "max_rate_hz": 120}
    value.update(overrides)
    return value


def wait_until(predicate, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError("Timed out waiting for simulated transport condition")


class FakeBackend:
    def __init__(self, count=5):
        self.devices = [(SimpleNamespace(address=f"00:11:22:33:44:{mid:02X}",
                                         marker_id=mid, name=f"ACE{mid}"), f"ACE{mid}")
                        for mid in range(1, count + 1)]
        self.lock = threading.Lock()
        self.records, self.clients = [], []
        self.block_write, self.fail_connect, self.no_clock = set(), set(), set()
        self.lose_ack_once = set()
        self.clock_offsets = {mid: 5_000_000_000 for mid in range(1, count + 1)}
        self.clock_epochs = {mid: 1000 + mid for mid in range(1, count + 1)}
        self.clock_delay, self.mtu, self.mtu_after_clock = {}, {}, {}
        self.connection_counts, self.read_counts = {}, {}
        self.scan_count = self.scans_active = self.max_scans_active = 0

    async def discover(self, timeout):
        self.scan_count += 1
        self.scans_active += 1
        self.max_scans_active = max(self.max_scans_active, self.scans_active)
        try:
            await asyncio.sleep(.005)
            return list(self.devices)
        finally:
            self.scans_active -= 1

    def make_client(self, device, disconnected_callback):
        if isinstance(device, str):
            raise AssertionError("An address would cause implicit per-link scanning")
        client = FakeClient(self, device, disconnected_callback)
        self.clients.append(client)
        return client

    def packets(self, marker_id=None, valid=None):
        with self.lock:
            return [record for record in self.records
                    if (marker_id is None or record[1] == marker_id)
                    and (valid is None or record[2].valid == valid)]

    def disconnect(self, marker_id):
        for client in self.clients:
            if client.device.marker_id == marker_id and client.is_connected:
                client.is_connected = False
                client.loop.call_soon_threadsafe(client.callback, client)


class FakeClient:
    def __init__(self, backend, device, callback):
        self.backend, self.device, self.callback = backend, device, callback
        self.is_connected = False
        self.loop, self.serial = None, None
        self.operations_active = self.max_operations_active = 0

    @property
    def mtu_size(self):
        return self.backend.mtu.get(self.device.marker_id, 247)

    async def connect(self):
        self.loop = asyncio.get_running_loop()
        mid = self.device.marker_id
        self.backend.connection_counts[mid] = self.backend.connection_counts.get(mid, 0) + 1
        self.serial = self.backend.connection_counts[mid]
        if mid in self.backend.fail_connect:
            raise ConnectionError("Simulated unavailable car")
        await asyncio.sleep(.001)
        self.is_connected = True

    async def disconnect(self):
        self.is_connected = False

    async def read_gatt_char(self, char, *, use_cached):
        if use_cached is not False:
            raise AssertionError("CLOCK must never be a cached GATT read")
        self.operations_active += 1
        self.max_operations_active = max(self.max_operations_active, self.operations_active)
        mid = self.device.marker_id
        self.backend.read_counts[mid] = self.backend.read_counts.get(mid, 0) + 1
        try:
            if mid in self.backend.no_clock:
                raise ConnectionError("Simulated absent ACT1 clock")
            await asyncio.sleep(self.backend.clock_delay.get(mid, .001))
            if mid in self.backend.mtu_after_clock:
                self.backend.mtu[mid] = self.backend.mtu_after_clock[mid]
            return pack_clock(self.backend.clock_epochs[mid], time.monotonic_ns() + self.backend.clock_offsets[mid])
        finally:
            self.operations_active -= 1

    async def write_gatt_char(self, char, data, response):
        if response is not True or len(data) != 40 or self.mtu_size < 43:
            raise AssertionError("ACP1 requires one 40-byte Write Request with ATT MTU >=43")
        packet = decode_pose(data)
        self.operations_active += 1
        self.max_operations_active = max(self.max_operations_active, self.operations_active)
        try:
            while self.device.marker_id in self.backend.block_write:
                await asyncio.sleep(.005)
            if not self.is_connected:
                raise ConnectionError("Simulated link dropped during write")
            with self.backend.lock:
                self.backend.records.append((self.serial, self.device.marker_id, packet))
            if self.device.marker_id in self.backend.lose_ack_once:
                self.backend.lose_ack_once.remove(self.device.marker_id)
                # The receiver accepted this packet, but its ATT response was
                # lost. The following connection keeps the same ACT1 epoch.
                raise asyncio.TimeoutError("Simulated lost acknowledgement after acceptance")
            await asyncio.sleep(0)
        finally:
            self.operations_active -= 1


class BackendTests(unittest.TestCase):
    def test_fresh_service_discovery_is_windows_only(self):
        backend = object.__new__(BleakBackend)
        device, callback = object(), object()
        for platform in ("win32", "linux"):
            with self.subTest(platform=platform), patch("aruco_ble_sender.sys.platform", platform):
                backend.client_type = Mock()
                backend.make_client(device, callback)
                options = {"winrt": {"use_cached_services": False}} if platform == "win32" else {}
                backend.client_type.assert_called_once_with(device, disconnected_callback=callback, **options)


class ConfigTests(unittest.TestCase):
    def test_rtt_100ms_allowed_and_300ms_age_limit_retained(self):
        defaults = load_config(config())
        self.assertEqual(defaults["max_clock_rtt_ms"], 100)
        self.assertEqual(defaults["max_sample_age_ms"], 300)
        self.assertEqual(load_config(config(max_clock_rtt_ms=100))["max_clock_rtt_ms"], 100)
        with self.assertRaises(ValueError):
            load_config(config(max_sample_age_ms=300.001))

    def test_subsets_explicit_vehicle_mapping(self):
        self.assertEqual(len(load_config(config(5))["cars"]), 5)
        value = config()
        value["cars"][0] = {"marker_id": 1, "vehicle_id": 54321, "name": "ACE54321"}
        self.assertEqual(load_config(value)["cars"][0]["vehicle_id"], 54321)

    def test_invalid_or_legacy_configuration_fails(self):
        invalids = [{"cars": []}, {"cars": [{"marker_id": 1, "enabled": False}]},
                    {"cars": [{"marker_id": 6, "name": "x"}]},
                    {"cars": [{"marker_id": 1, "name": "same"}, {"marker_id": 2, "name": "same"}]},
                    {"cars": [{"marker_id": 1, "name": "x"}, {"marker_id": 1, "name": "y"}]},
                    {"cars": [{"marker_id": 1, "vehicle_id": 8, "name": "x"},
                              {"marker_id": 2, "vehicle_id": 8, "name": "y"}]},
                    {"cars": [{"marker_id": 1, "address": "wrong"}]},
                    config(rx_uuid="legacy"), config(max_sample_age_ms=301),
                    config(max_clock_rtt_ms=100.001), config(max_rate_hz=float("nan")),
                    config(initial_clock_samples=1), config(enabled=False)]
        for value in invalids:
            with self.subTest(value=value), self.assertRaises(ValueError):
                load_config(value)


class SenderTests(unittest.TestCase):
    def setUp(self):
        self.senders = []

    def tearDown(self):
        for sender in self.senders:
            sender.stop()

    def start(self, settings=None, backend=None, wait=True, *, clock_ns=time.monotonic_ns):
        backend = backend or FakeBackend()
        sender = BLEPoseSender(settings or config(), backend=backend, clock_ns=clock_ns)
        self.senders.append(sender)
        sender.start()
        if wait:
            wait_until(lambda: all(car["state"] == "ready" for car in sender.snapshot()["cars"].values()))
        return sender, backend

    def pose(self, value=100):
        return {"x_mm": value, "y_mm": 200.6, "heading_deg": 359.25}

    def test_five_links_shared_scan_independent_routing_and_clock(self):
        settings = config(5)
        settings["cars"][0]["vehicle_id"] = 101
        sender, backend = self.start(settings)
        sender.publish_frame(1, time.monotonic_ns(), {mid: self.pose(mid * 100) for mid in range(1, 6)})
        wait_until(lambda: len(backend.packets(valid=True)) == 5)
        self.assertEqual(backend.scan_count, 1)
        self.assertEqual(backend.max_scans_active, 1)
        for mid in range(1, 6):
            packet = backend.packets(mid)[0][2]
            self.assertEqual(packet.vehicle_id, 101 if mid == 1 else mid)
            self.assertEqual(packet.epoch, 1000 + mid)
            self.assertEqual((packet.x_mm, packet.y_mm, packet.heading_cdeg), (mid * 100, 201, 35925))
            self.assertEqual(backend.read_counts[mid], 3)
        self.assertTrue(all(client.max_operations_active == 1 for client in backend.clients))

    def test_process_restart_keeps_sequence_with_same_receiver_epoch(self):
        with tempfile.TemporaryDirectory() as folder:
            state_path = Path(folder) / "sequence.json"
            backend = FakeBackend()
            previous = 0
            for run in range(2):
                sender = BLEPoseSender(config(), backend=backend, sequence_state_path=state_path)
                self.senders.append(sender)
                sender.start()
                wait_until(lambda: sender.snapshot()["cars"][1]["state"] == "ready")
                sender.publish_frame(1, time.monotonic_ns(), {1: self.pose()})
                wait_until(lambda: len(backend.packets(1)) == run + 1)
                current = backend.packets(1)[-1][2].sequence
                self.assertGreater(current, previous)
                previous = current
                sender.stop()
            backend.clock_epochs[1] += 1
            sender = BLEPoseSender(config(), backend=backend, sequence_state_path=state_path)
            self.senders.append(sender)
            sender.start()
            wait_until(lambda: sender.snapshot()["cars"][1]["state"] == "ready")
            sender.publish_frame(1, time.monotonic_ns(), {1: self.pose()})
            wait_until(lambda: len(backend.packets(1)) == 3)
            self.assertEqual(backend.packets(1)[-1][2].sequence, 1)
            sender.stop()
            self.assertNotIn("x_mm", state_path.read_text())

    def test_newest_slot_replaces_backlog(self):
        now_ns = [time.monotonic_ns()]
        sender, backend = self.start(config(max_rate_hz=5), clock_ns=lambda: now_ns[0])
        sender.publish_frame(1, now_ns[0], {1: self.pose(1)})
        wait_until(lambda: len(backend.packets(1)) == 1
                   and sender.snapshot()["cars"][1]["sent_pose"] == 1)

        # Hold the send period while newer frames replace the pending slot.
        now_ns[0] += 1_000_000
        for frame in range(2, 102):
            sender.publish_frame(frame, now_ns[0], {1: self.pose(frame)})
        status = sender.snapshot()["cars"][1]
        self.assertEqual(status["pending"], 1)
        self.assertEqual(status["replaced"], 99)
        self.assertEqual(len(backend.packets(1)), 1)

        # Reach 200 ms without letting scheduler delays expire the sample.
        now_ns[0] += 199_000_000
        wait_until(lambda: sender.snapshot()["cars"][1]["sent_pose"] == 2)
        self.assertEqual([p.x_mm for _, _, p in backend.packets(1)], [1, 101])
        self.assertEqual(sender.snapshot()["cars"][1]["dropped"], 0)

    def test_disconnected_samples_not_replayed_same_epoch_sequence_continues(self):
        backend = FakeBackend()
        backend.fail_connect.add(1)
        sender, backend = self.start(config(), backend, wait=False)
        sender.publish_frame(1, time.monotonic_ns(), {1: self.pose(111)})
        wait_until(lambda: sender.snapshot()["cars"][1]["errors"] > 0)
        backend.fail_connect.clear()
        wait_until(lambda: sender.snapshot()["cars"][1]["state"] == "ready")
        self.assertEqual(backend.packets(), [])
        sender.publish_frame(2, time.monotonic_ns(), {1: self.pose(222)})
        wait_until(lambda: len(backend.packets(1)) == 1)
        backend.disconnect(1)
        wait_until(lambda: sender.snapshot()["cars"][1]["state"] != "ready")
        sender.publish_frame(3, time.monotonic_ns(), {1: self.pose(333)})
        wait_until(lambda: sender.snapshot()["cars"][1]["state"] == "ready")
        self.assertEqual(len(backend.packets(1)), 1)
        sender.publish_frame(4, time.monotonic_ns(), {1: self.pose(444)})
        wait_until(lambda: len(backend.packets(1)) == 2)
        self.assertEqual([p.x_mm for _, _, p in backend.packets(1)], [222, 444])
        self.assertEqual([p.sequence for _, _, p in backend.packets(1)], [1, 2])

    def test_capture_before_ready_old_future_duplicate_do_not_send(self):
        before = time.monotonic_ns()
        sender, backend = self.start(config(max_sample_age_ms=80))
        sender.publish_frame(1, before, {1: self.pose()})
        wait_until(lambda: sender.snapshot()["cars"][1]["dropped"] == 1)
        sender.publish_frame(2, time.monotonic_ns() - 1_000_000_000, {})
        wait_until(lambda: sender.snapshot()["cars"][1]["dropped"] == 2)
        sender.publish_frame(3, time.monotonic_ns() + 1_000_000_000, {1: self.pose()})
        wait_until(lambda: sender.snapshot()["cars"][1]["dropped"] == 3)
        self.assertEqual(backend.packets(), [])
        sender.publish_frame(4, time.monotonic_ns(), {1: self.pose(444)})
        wait_until(lambda: len(backend.packets(1)) == 1)
        self.assertFalse(sender.publish_frame(4, time.monotonic_ns(), {1: self.pose(999)}))
        time.sleep(.1)
        self.assertEqual(len(backend.packets(1)), 1)

    def test_lost_ack_same_epoch_reconnect_does_not_reuse_sequence(self):
        sender, backend = self.start()
        backend.lose_ack_once.add(1)
        sender.publish_frame(1, time.monotonic_ns(), {1: self.pose(111)})
        wait_until(lambda: len(backend.packets(1)) == 1)
        accepted = backend.packets(1)[0][2]
        wait_until(lambda: backend.connection_counts.get(1, 0) >= 2 and
                   sender.snapshot()["cars"][1]["state"] == "ready")
        self.assertEqual(sender.snapshot()["cars"][1]["sent_pose"], 0)
        sender.publish_frame(2, time.monotonic_ns(), {1: self.pose(222)})
        wait_until(lambda: len(backend.packets(1)) == 2)
        following = backend.packets(1)[1][2]
        self.assertEqual((accepted.epoch, following.epoch), (1001, 1001))
        self.assertEqual((accepted.sequence, following.sequence), (1, 2))
        self.assertGreater(following.capture_pi_ns, accepted.capture_pi_ns)
        self.assertEqual(following.x_mm, 222)

    def test_lost_ack_preserves_capture_floor_across_same_epoch_reconnect(self):
        sender, backend = self.start()
        backend.lose_ack_once.add(1)
        sender.publish_frame(1, time.monotonic_ns(), {1: self.pose(111)})
        wait_until(lambda: len(backend.packets(1)) == 1)
        backend.clock_offsets[1] -= 2_000_000_000
        wait_until(lambda: backend.connection_counts.get(1, 0) >= 2 and
                   sender.snapshot()["cars"][1]["state"] == "ready")
        sender.publish_frame(2, time.monotonic_ns(), {1: self.pose(222)})
        wait_until(lambda: sender.snapshot()["cars"][1]["dropped"] > 0)
        self.assertEqual(len(backend.packets(1)), 1)
        self.assertEqual(sender.snapshot()["cars"][1]["observation_state"], "timestamp_not_advanced")

    def test_invalid_only_from_new_frames_no_fabricated_timestamp(self):
        sender, backend = self.start()
        self.assertEqual(backend.packets(), [])
        sender.publish_frame(1, time.monotonic_ns(), {})
        wait_until(lambda: len(backend.packets(1, valid=False)) == 1)
        packet = backend.packets(1)[0][2]
        self.assertEqual((packet.flags, packet.x_mm, packet.y_mm, packet.heading_cdeg), (0, 0, 0, 0))
        sender.invalidate_all("camera_unavailable")
        time.sleep(.03)
        self.assertEqual(len(backend.packets(1)), 1)
        sender.publish_frame(2, time.monotonic_ns(), {1: self.pose()})
        wait_until(lambda: len(backend.packets(1, valid=True)) == 1)
        self.assertGreater(backend.packets(1)[-1][2].capture_pi_ns, packet.capture_pi_ns)

    def test_slow_or_failed_car_does_not_block_others(self):
        backend = FakeBackend()
        backend.fail_connect.add(5)
        sender, backend = self.start(config(5), backend, wait=False)
        wait_until(lambda: all(sender.snapshot()["cars"][mid]["state"] == "ready" for mid in (1, 2, 3, 4)))
        backend.block_write.add(1)
        sender.publish_frame(1, time.monotonic_ns(), {mid: self.pose(mid) for mid in range(1, 6)})
        wait_until(lambda: len(backend.packets(2)) == 1)
        self.assertEqual(backend.packets(1), [])
        self.assertEqual(backend.packets(5), [])
        wait_until(lambda: sender.snapshot()["cars"][1]["errors"] > 0)
        sender.publish_frame(2, time.monotonic_ns(), {2: self.pose(222)})
        wait_until(lambda: len(backend.packets(2, valid=True)) == 2)

    def test_mtu_negotiates_from_23_after_clock_before_first_pose(self):
        backend = FakeBackend()
        backend.mtu[1] = 23
        backend.mtu_after_clock[1] = 517
        sender, backend = self.start(config(), backend)
        self.assertEqual(backend.read_counts[1], 3)
        self.assertEqual(sender.snapshot()["cars"][1]["mtu"], 517)
        self.assertEqual(backend.packets(), [])
        sender.publish_frame(1, time.monotonic_ns(), {1: self.pose()})
        wait_until(lambda: len(backend.packets(1)) == 1)
        self.assertEqual(backend.connection_counts[1], 1)

    def test_mtu_drop_before_write_does_not_send_pose(self):
        sender, backend = self.start()
        backend.mtu[1] = 23
        sender.publish_frame(1, time.monotonic_ns(), {1: self.pose()})
        wait_until(lambda: sender.snapshot()["cars"][1]["errors"] > 0)
        self.assertEqual(backend.packets(), [])

    def test_bad_clock_or_mtu_gates_only_affected_link(self):
        for condition in ("no_clock", "small_mtu", "slow_clock"):
            with self.subTest(condition=condition):
                backend = FakeBackend()
                if condition == "no_clock": backend.no_clock.add(1)
                if condition == "small_mtu": backend.mtu[1] = 23
                if condition == "slow_clock": backend.clock_delay[1] = .12
                sender, backend = self.start(config(2, sync_timeout_s=.3), backend, wait=False)
                wait_until(lambda: sender.snapshot()["cars"][2]["state"] == "ready")
                sender.publish_frame(1, time.monotonic_ns(), {1: self.pose(), 2: self.pose()})
                wait_until(lambda: len(backend.packets(2)) == 1)
                wait_until(lambda: sender.snapshot()["cars"][1]["errors"] > 0)
                self.assertEqual(backend.packets(1), [])
                self.assertFalse(sender.snapshot()["cars"][1]["synchronized"])
                sender.stop()

    def test_refresh_backstep_is_skipped_not_clamped_new_epoch_resets(self):
        sender, backend = self.start(config(sync_interval_s=.03))
        sender.publish_frame(1, time.monotonic_ns(), {1: self.pose(111)})
        wait_until(lambda: len(backend.packets(1)) == 1)
        old = backend.packets(1)[0][2]
        backend.clock_offsets[1] -= 2_000_000_000
        wait_until(lambda: sender.snapshot()["cars"][1].get("clock_offset_ns", 5e9) < 4e9)
        sender.publish_frame(2, time.monotonic_ns(), {1: self.pose(222)})
        wait_until(lambda: sender.snapshot()["cars"][1]["dropped"] > 0)
        self.assertEqual(len(backend.packets(1)), 1)
        backend.clock_epochs[1] = 9999
        wait_until(lambda: sender.snapshot()["cars"][1]["epoch"] == 9999)
        time.sleep(.01)
        sender.publish_frame(3, time.monotonic_ns(), {1: self.pose(333)})
        wait_until(lambda: len(backend.packets(1)) == 2)
        new = backend.packets(1)[1][2]
        self.assertEqual((new.epoch, new.sequence, new.x_mm), (9999, 1, 333))
        self.assertLess(new.capture_pi_ns, old.capture_pi_ns)

    def test_shutdown_disconnects_clients(self):
        sender, backend = self.start(config(2))
        sender.stop()
        self.assertFalse(sender.snapshot()["running"])
        self.assertTrue(all(not client.is_connected for client in backend.clients))

    def test_duplicate_advertised_name_refuses_ambiguous_target(self):
        backend = FakeBackend(2)
        backend.devices[1] = (backend.devices[1][0], "ACE1")
        settings = config()
        settings["cars"][0] = {"marker_id": 1, "name": "ACE1"}
        sender, backend = self.start(settings, backend, wait=False)
        wait_until(lambda: "Multiple devices" in sender.snapshot()["cars"][1]["last_error"])
        self.assertEqual(backend.clients, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
