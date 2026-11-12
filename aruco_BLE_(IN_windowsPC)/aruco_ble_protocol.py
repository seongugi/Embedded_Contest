"""Actual Pi receiver protocol: ACP1 pose writes (40 B), ACT1 clock reads (20 B).

This replaces the preliminary 20-byte ArUco protocol completely. PC timestamps
must come from time.monotonic_ns(), then be translated into the Pi monotonic
clock before packing. There is no HELLO, CLOCK_MODEL, notification, or CRC.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from numbers import Integral
import struct

SERVICE_UUID = "7b9c0001-6a54-4f1b-9f65-2c7b8d1e4a90"
POSE_CHARACTERISTIC_UUID = "7b9c0002-6a54-4f1b-9f65-2c7b8d1e4a90"
CLOCK_CHARACTERISTIC_UUID = "7b9c0003-6a54-4f1b-9f65-2c7b8d1e4a90"
POSE_MAGIC = b"ACP1"
CLOCK_MAGIC = b"ACT1"
POSE_STRUCT = struct.Struct("<4sHHQIQiiHH")
CLOCK_STRUCT = struct.Struct("<4sQQ")
POSE_PACKET_SIZE = POSE_STRUCT.size
CLOCK_PACKET_SIZE = CLOCK_STRUCT.size
MIN_ATT_MTU = POSE_PACKET_SIZE + 3
FLAG_VALID = 1
POSITION_LIMIT_MM = 1_000_000
NS_PER_MS = 1_000_000
DEFAULT_MAX_RTT_MS = 100.0
DEFAULT_MAX_AGE_MS = 300.0
CLOCK_MARGIN_NS = 1_000_000


def _uint(value: int, bits: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, Integral):
        raise ValueError(f"{name} must be an unsigned {bits}-bit integer")
    value = int(value)
    if not 0 <= value < (1 << bits):
        raise ValueError(f"{name} is outside unsigned {bits}-bit range")
    return value


def _finite(value: float, name: str) -> float:
    if isinstance(value, (str, bytes, bool)):
        raise ValueError(f"{name} must be a finite number")
    try:
        value = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{name} must be a finite number") from exc
    if not math.isfinite(value):
        raise ValueError(f"{name} must be finite")
    return value


def _vehicle_id(value: int) -> int:
    value = _uint(value, 16, "vehicle_id")
    if value == 0:
        raise ValueError("vehicle_id must be in 1..65535")
    return value


def _epoch(value: int) -> int:
    value = _uint(value, 64, "epoch")
    if value == 0:
        raise ValueError("epoch must not be zero")
    return value


def _pi_timestamp(value: int, name: str) -> int:
    value = _uint(value, 64, name)
    if value == 0:
        raise ValueError(f"{name} must not be zero")
    return value


def round_mm(value: float) -> int:
    """Round with Python round(), then enforce the real receiver's +/-1e6 mm."""
    value = round(_finite(value, "coordinate_mm"))
    if not -POSITION_LIMIT_MM <= value <= POSITION_LIMIT_MM:
        raise ValueError("rounded coordinate exceeds receiver limit of +/-1000000 mm")
    return value


def heading_centidegrees(heading_deg: float) -> int:
    """Receiver heading convention must already be applied: +Y=0, +X=90."""
    heading_deg = _finite(heading_deg, "heading_deg")
    return round((heading_deg % 360.0) * 100.0) % 36000


@dataclass(frozen=True)
class PosePacket:
    magic: bytes
    vehicle_id: int
    flags: int
    epoch: int
    sequence: int
    capture_pi_ns: int
    x_mm: int
    y_mm: int
    heading_cdeg: int
    reserved: int

    @property
    def valid(self) -> bool:
        return self.flags == FLAG_VALID

    @property
    def heading_deg(self) -> float:
        return self.heading_cdeg / 100.0


@dataclass(frozen=True)
class ClockPacket:
    epoch: int
    pi_ns: int


def pack_pose(
    vehicle_id: int,
    sequence: int,
    capture_pi_ns: int,
    x_mm: float,
    y_mm: float,
    heading_deg: float,
    *,
    epoch: int,
    valid: bool = True,
) -> bytes:
    """Pack ONE 40-byte write. x/y are mm; heading is already vehicle heading.

    valid=False is an explicit observation/status invalidation; its coordinate
    fields are always zero, never a repeated old position. It still requires a
    fresh timestamp, advancing sequence, and the current nonzero clock epoch.
    """
    if not isinstance(valid, bool):
        raise ValueError("valid must be bool")
    vehicle_id = _vehicle_id(vehicle_id)
    epoch = _epoch(epoch)
    sequence = _uint(sequence, 32, "sequence")
    capture_pi_ns = _pi_timestamp(capture_pi_ns, "capture_pi_ns")
    if valid:
        x, y, heading = round_mm(x_mm), round_mm(y_mm), heading_centidegrees(heading_deg)
    else:
        x, y, heading = 0, 0, 0
    return POSE_STRUCT.pack(
        POSE_MAGIC, vehicle_id, int(valid), epoch, sequence,
        capture_pi_ns, x, y, heading, 0,
    )


def pack_invalid(vehicle_id: int, sequence: int, capture_pi_ns: int, *, epoch: int) -> bytes:
    return pack_pose(vehicle_id, sequence, capture_pi_ns, 0, 0, 0, epoch=epoch, valid=False)


def pack_clock(epoch: int, pi_ns: int) -> bytes:
    """Receiver/probe helper. ACT1 is the value returned by a GATT CLOCK read."""
    return CLOCK_STRUCT.pack(CLOCK_MAGIC, _epoch(epoch), _pi_timestamp(pi_ns, "pi_ns"))


def _bytes(data: bytes | bytearray | memoryview, size: int) -> bytes:
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise ValueError("packet must be bytes, bytearray, or memoryview")
    data = bytes(data)
    if len(data) != size:
        raise ValueError(f"packet must contain exactly {size} bytes")
    return data


def decode_pose(data: bytes | bytearray | memoryview) -> PosePacket:
    packet = PosePacket(*POSE_STRUCT.unpack(_bytes(data, POSE_PACKET_SIZE)))
    if packet.magic != POSE_MAGIC:
        raise ValueError("expected ACP1 pose magic")
    _vehicle_id(packet.vehicle_id)
    _epoch(packet.epoch)
    _pi_timestamp(packet.capture_pi_ns, "capture_pi_ns")
    if packet.flags not in (0, FLAG_VALID):
        raise ValueError("flags must be 0 (invalid) or 1 (valid)")
    if packet.reserved != 0:
        raise ValueError("reserved field must be zero")
    if not 0 <= packet.heading_cdeg < 36000:
        raise ValueError("heading_cdeg must be in 0..35999")
    if abs(packet.x_mm) > POSITION_LIMIT_MM or abs(packet.y_mm) > POSITION_LIMIT_MM:
        raise ValueError("coordinate exceeds receiver limit of +/-1000000 mm")
    if not packet.valid and (packet.x_mm or packet.y_mm or packet.heading_cdeg):
        raise ValueError("invalid pose must have zero coordinate and heading fields")
    return packet


def decode_clock(data: bytes | bytearray | memoryview) -> ClockPacket:
    magic, epoch, pi_ns = CLOCK_STRUCT.unpack(_bytes(data, CLOCK_PACKET_SIZE))
    if magic != CLOCK_MAGIC:
        raise ValueError("expected ACT1 clock magic")
    return ClockPacket(_epoch(epoch), _pi_timestamp(pi_ns, "pi_ns"))


def decode_packet(data: bytes | bytearray | memoryview) -> PosePacket | ClockPacket:
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise ValueError("packet must be bytes, bytearray, or memoryview")
    magic = bytes(data[:4])
    if magic == POSE_MAGIC:
        return decode_pose(data)
    if magic == CLOCK_MAGIC:
        return decode_clock(data)
    raise ValueError("unknown packet magic; expected ACP1 or ACT1")


@dataclass(frozen=True)
class ClockSample:
    epoch: int
    offset_ns: int
    round_trip_ns: int
    sampled_pc_ns: int

    def map_capture(self, capture_pc_ns: int) -> int:
        """Translate original PC observation time, never replace it with 'now'."""
        result = _uint(capture_pc_ns, 64, "capture_pc_ns") + self.offset_ns
        return _pi_timestamp(result, "capture_pi_ns")


def calculate_clock_sample(
    t0: int,
    t1: int,
    clock: ClockPacket,
    max_rtt_ms: float = DEFAULT_MAX_RTT_MS,
) -> ClockSample:
    """Build Pi-minus-PC offset from an uncached ACT1 GATT read.

    t0 is PC monotonic_ns immediately BEFORE read; t1 is immediately AFTER.
    Pi must sample pi_ns while servicing this read. Subtracting t1 and a 1 ms
    margin intentionally biases the translated observation older. This is not
    symmetric-delay estimation, NTP discipline, or a guarantee against drift.
    """
    t0 = _uint(t0, 64, "t0")
    t1 = _uint(t1, 64, "t1")
    if not isinstance(clock, ClockPacket):
        raise ValueError("clock must be a decoded ClockPacket")
    _epoch(clock.epoch)
    _pi_timestamp(clock.pi_ns, "pi_ns")
    max_rtt_ms = _finite(max_rtt_ms, "max_rtt_ms")
    if max_rtt_ms <= 0:
        raise ValueError("max_rtt_ms must be positive")
    round_trip_ns = t1 - t0
    if round_trip_ns < 0:
        raise ValueError("PC monotonic time moved backwards")
    if round_trip_ns > max_rtt_ms * NS_PER_MS:
        raise ValueError("CLOCK read RTT exceeds the allowed limit")
    return ClockSample(clock.epoch, clock.pi_ns - t1 - CLOCK_MARGIN_NS, round_trip_ns, t1)


def is_newer_sequence(candidate: int, previous: int) -> bool:
    candidate = _uint(candidate, 32, "candidate")
    previous = _uint(previous, 32, "previous")
    return 0 < ((candidate - previous) & 0xFFFFFFFF) < 0x80000000


def is_fresh_capture(capture_pi_ns: int, now_pi_ns: int, max_age_ms: float = DEFAULT_MAX_AGE_MS) -> bool:
    """Actual receiver freshness boundary: future or age >= 300 ms is invalid."""
    capture_pi_ns = _uint(capture_pi_ns, 64, "capture_pi_ns")
    now_pi_ns = _uint(now_pi_ns, 64, "now_pi_ns")
    if capture_pi_ns == 0 or now_pi_ns == 0:
        return False
    max_age_ms = _finite(max_age_ms, "max_age_ms")
    if max_age_ms <= 0:
        raise ValueError("max_age_ms must be positive")
    return 0 <= now_pi_ns - capture_pi_ns < max_age_ms * NS_PER_MS


class SequenceTracker:
    """uint32 sequence helper; capture timestamp must ALSO advance separately.

    Reset only when the epoch changes, never merely on a CLOCK read or reconnect.
    Preserve sequence when reconnecting to the same epoch. This helper does
    not by itself validate epoch, vehicle, timestamp age, or a complete packet.
    """

    def __init__(self) -> None:
        self.last_sequence: int | None = None

    def reset(self) -> None:
        self.last_sequence = None

    def accept(self, sequence: int) -> bool:
        sequence = _uint(sequence, 32, "sequence")
        if self.last_sequence is not None and not is_newer_sequence(sequence, self.last_sequence):
            return False
        self.last_sequence = sequence
        return True
