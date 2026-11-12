"""Offline tests for the ACT1/ACP1 protocol implemented by the real Pi receiver."""

import math
import struct
import unittest

from aruco_ble_protocol import (
    CLOCK_PACKET_SIZE, MIN_ATT_MTU, POSE_PACKET_SIZE, POSITION_LIMIT_MM,
    ClockPacket, SequenceTracker, calculate_clock_sample, decode_clock,
    decode_packet, decode_pose, heading_centidegrees, is_fresh_capture,
    is_newer_sequence, pack_clock, pack_invalid, pack_pose,
)


class WireTests(unittest.TestCase):
    def test_pose_exact_receiver_layout_golden_bytes(self):
        wire = pack_pose(0x1234, 0xA1A2A3A4, 0x1112131415161718, 1234.6, -2500, 90, epoch=0x0102030405060708)
        self.assertEqual(wire.hex(), "41435031341201000807060504030201a4a3a2a11817161514131211d30400003cf6ffff28230000")
        packet = decode_pose(wire)
        self.assertEqual((packet.vehicle_id, packet.sequence), (0x1234, 0xA1A2A3A4))
        self.assertEqual((packet.x_mm, packet.y_mm, packet.heading_cdeg), (1235, -2500, 9000))
        self.assertEqual((packet.flags, packet.reserved, packet.valid), (1, 0, True))
        self.assertEqual(packet.heading_deg, 90.0)
        self.assertEqual((POSE_PACKET_SIZE, CLOCK_PACKET_SIZE, MIN_ATT_MTU), (40, 20, 43))
        self.assertEqual(decode_packet(wire), packet)

    def test_clock_exact_layout_and_nonzero_epoch(self):
        wire = pack_clock(0x0102030405060708, 0x1112131415161718)
        self.assertEqual(wire.hex(), "4143543108070605040302011817161514131211")
        self.assertEqual(decode_clock(wire), ClockPacket(0x0102030405060708, 0x1112131415161718))
        self.assertEqual(decode_packet(wire), decode_clock(wire))
        with self.assertRaises(ValueError):
            pack_clock(0, 1)
        with self.assertRaises(ValueError):
            decode_clock(struct.pack("<4sQQ", b"ACT1", 0, 1))

    def test_zero_pi_timestamps_match_real_receiver_rejection(self):
        with self.assertRaises(ValueError):
            pack_pose(1, 1, 0, 1, 2, 3, epoch=1)
        with self.assertRaises(ValueError):
            pack_invalid(1, 1, 0, epoch=1)
        with self.assertRaises(ValueError):
            pack_clock(1, 0)
        with self.assertRaises(ValueError):
            decode_clock(struct.pack("<4sQQ", b"ACT1", 1, 0))
        fields = list(struct.unpack("<4sHHQIQiiHH", pack_pose(1, 1, 1, 1, 2, 3, epoch=1)))
        fields[5] = 0
        with self.assertRaises(ValueError):
            decode_pose(struct.pack("<4sHHQIQiiHH", *fields))
        with self.assertRaises(ValueError):
            calculate_clock_sample(1, 2, ClockPacket(1, 0))
        self.assertFalse(is_fresh_capture(0, 0))
        self.assertFalse(is_fresh_capture(0, 1))
        self.assertFalse(is_fresh_capture(1, 0))

    def test_heading_quadrants_rounding_and_wrap(self):
        for heading, expected in ((0, 0), (90, 9000), (180, 18000), (270, 27000), (-90, 27000), (360, 0), (720, 0), (359.999, 0), (45.126, 4513)):
            with self.subTest(heading=heading):
                self.assertEqual(heading_centidegrees(heading), expected)
        for value in (math.nan, math.inf, -math.inf, "90", True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                heading_centidegrees(value)

    def test_invalid_frame_zeros_values_but_preserves_original_time_and_sequence(self):
        packet = decode_pose(pack_invalid(5, 99, 4567890123, epoch=8))
        self.assertFalse(packet.valid)
        self.assertEqual((packet.vehicle_id, packet.sequence, packet.capture_pi_ns, packet.epoch), (5, 99, 4567890123, 8))
        self.assertEqual((packet.x_mm, packet.y_mm, packet.heading_cdeg), (0, 0, 0))
        # It cannot accidentally put the last valid coordinates in an invalid frame.
        packet = decode_pose(pack_pose(1, 1, 3, 999, 888, 45, epoch=2, valid=False))
        self.assertEqual((packet.x_mm, packet.y_mm, packet.heading_cdeg), (0, 0, 0))

    def test_actual_receiver_coordinate_limits_and_nonfinite_rejection(self):
        packet = decode_pose(pack_pose(1, 1, 1, POSITION_LIMIT_MM, -POSITION_LIMIT_MM, 0, epoch=1))
        self.assertEqual((packet.x_mm, packet.y_mm), (1_000_000, -1_000_000))
        for value in (1_000_001, -1_000_001, math.nan, math.inf, -math.inf, 1e100, "1"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                pack_pose(1, 0, 1, value, 0, 0, epoch=1)

    def test_header_uint_bounds(self):
        for vehicle in (0, 65536, -1, True):
            with self.subTest(vehicle=vehicle), self.assertRaises(ValueError):
                pack_pose(vehicle, 0, 1, 0, 0, 0, epoch=1)
        for sequence in (-1, 1 << 32):
            with self.subTest(sequence=sequence), self.assertRaises(ValueError):
                pack_pose(1, sequence, 1, 0, 0, 0, epoch=1)
        for timestamp in (-1, 1 << 64):
            with self.subTest(timestamp=timestamp), self.assertRaises(ValueError):
                pack_pose(1, 0, timestamp, 0, 0, 0, epoch=1)
        with self.assertRaises(ValueError):
            pack_pose(1, 0, 1, 0, 0, 0, epoch=0)
        with self.assertRaises(ValueError):
            pack_pose(1, 0, 1, 0, 0, 0, epoch=1, valid=2)

    def test_rejects_malformed_old_and_unsupported_frames(self):
        valid = pack_pose(1, 0, 1, 0, 0, 0, epoch=1)
        cases = [valid[:-1], valid + b"\0", b"ACP2" + valid[4:], bytes(20)]
        fields = list(struct.unpack("<4sHHQIQiiHH", valid))
        for index, value in ((1, 0), (2, 2), (3, 0), (6, 1_000_001), (8, 36000), (9, 1)):
            modified = fields.copy()
            modified[index] = value
            cases.append(struct.pack("<4sHHQIQiiHH", *modified))
        invalid_with_position = fields.copy()
        invalid_with_position[2], invalid_with_position[6] = 0, 1
        cases.append(struct.pack("<4sHHQIQiiHH", *invalid_with_position))
        for wire in cases:
            with self.subTest(wire=wire.hex()), self.assertRaises(ValueError):
                decode_pose(wire)
        for wire in (pack_clock(1, 1)[:-1], pack_clock(1, 1) + b"\0", b"ACT2" + pack_clock(1, 1)[4:]):
            with self.assertRaises(ValueError):
                decode_clock(wire)


class ClockTests(unittest.TestCase):
    def test_read_completion_offset_is_conservative_under_asymmetry(self):
        actual_offset = 7_000_000_000
        pc_before = 10_000_000_000
        forward_ns, reverse_ns = 2_000_000, 18_000_000
        clock = ClockPacket(88, pc_before + forward_ns + actual_offset)
        pc_after = pc_before + forward_ns + reverse_ns
        sample = calculate_clock_sample(pc_before, pc_after, clock)
        self.assertEqual(sample.epoch, 88)
        self.assertEqual(sample.round_trip_ns, 20_000_000)
        self.assertEqual(sample.sampled_pc_ns, pc_after)
        self.assertEqual(sample.offset_ns, actual_offset - reverse_ns - 1_000_000)
        pc_capture = pc_before + 100_000_000
        self.assertLess(sample.map_capture(pc_capture), pc_capture + actual_offset)
        self.assertEqual(sample.map_capture(pc_capture), pc_capture + sample.offset_ns)

    def test_exact_100ms_rtt_allowed_above_rejected(self):
        calculate_clock_sample(1_000_000_000, 1_100_000_000, ClockPacket(1, 2_000_000_000))
        with self.assertRaisesRegex(ValueError, "RTT"):
            calculate_clock_sample(1_000_000_000, 1_100_000_001, ClockPacket(1, 2_000_000_000))
        with self.assertRaisesRegex(ValueError, "backwards"):
            calculate_clock_sample(10, 9, ClockPacket(1, 10))

    def test_time_refresh_can_move_mapping_back_and_must_not_be_clamped(self):
        old = calculate_clock_sample(1_000_000_000, 1_002_000_000, ClockPacket(1, 2_001_000_000))
        new = calculate_clock_sample(1_100_000_000, 1_120_000_000, ClockPacket(1, 2_101_000_000))
        previous_capture = old.map_capture(1_200_000_000)
        next_capture = new.map_capture(1_201_000_000)
        self.assertLess(next_capture, previous_capture)
        # Caller must skip until capture time advances, not change measurement time.
        self.assertEqual(next_capture, 2_181_000_000)

    def test_capture_freshness_exact_receiver_boundary(self):
        now = 5_000_000_000
        self.assertTrue(is_fresh_capture(now, now))
        self.assertTrue(is_fresh_capture(now - 299_999_999, now))
        self.assertFalse(is_fresh_capture(now - 300_000_000, now))
        self.assertFalse(is_fresh_capture(now + 1, now))


class SequenceTests(unittest.TestCase):
    def test_uint32_wrap_duplicates_late_and_reset(self):
        tracker = SequenceTracker()
        self.assertTrue(tracker.accept(0xFFFFFFFE))
        self.assertTrue(tracker.accept(0xFFFFFFFF))
        self.assertTrue(tracker.accept(0))
        self.assertFalse(tracker.accept(0))
        self.assertFalse(tracker.accept(0xFFFFFFFF))
        self.assertTrue(tracker.accept(2))
        self.assertFalse(tracker.accept(1))
        tracker.reset()
        self.assertTrue(tracker.accept(1))

    def test_half_range_is_not_newer(self):
        self.assertFalse(is_newer_sequence(0x80000000, 0))
        self.assertFalse(is_newer_sequence(0, 0x80000000))
        self.assertTrue(is_newer_sequence(0x7FFFFFFF, 0))


if __name__ == "__main__":
    unittest.main()
