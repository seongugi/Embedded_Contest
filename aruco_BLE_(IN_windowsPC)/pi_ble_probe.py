"""Temporary Linux/BlueZ ACP1/ACT1 probe; synthetic data only, NO vehicle IPC.

Requires dbus-next in an isolated environment. Registers temporary objects on
the existing system BlueZ daemon and unregisters them on exit. Does not change
adapter settings, pairing, Wi-Fi, NTP, or the real vehicle application.
"""

import argparse
import asyncio
from collections import Counter
import ctypes
import json
import math
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import socket
import struct
import time

from dbus_next import BusType, DBusError, Message, Variant
from dbus_next.aio import MessageBus
from dbus_next.constants import PropertyAccess, MessageType
from dbus_next.service import ServiceInterface, dbus_property, method

from aruco_ble_protocol import (
    SERVICE_UUID, POSE_CHARACTERISTIC_UUID, CLOCK_CHARACTERISTIC_UUID,
    MIN_ATT_MTU, POSE_PACKET_SIZE, SequenceTracker, decode_pose,
    is_fresh_capture, is_newer_sequence, pack_clock,
)

ROOT = "/org/aruco/probe"
SERVICE = ROOT + "/service0"
POSE = SERVICE + "/char0"
CLOCK = SERVICE + "/char1"
ADV = "/org/aruco/probe_advertisement"
KNOWN_POSES = ((123, 456, 1250), (988, 654, 31500))


def distribution(values):
    if not values:
        return {"count": 0}
    values = sorted(values)
    return {"count": len(values), "min": values[0], "max": values[-1],
            "mean": sum(values) / len(values),
            "p50": values[round((len(values) - 1) * .5)],
            "p95": values[round((len(values) - 1) * .95)]}


def option(options, name, default=None):
    value = options.get(name)
    return default if value is None else value.value


class ProbeState:
    def __init__(self):
        self.counts = Counter()
        self.sequence = SequenceTracker()
        self.epoch = secrets.randbits(64) or 1
        self.epoch_history = [self.epoch]
        self.peer = None
        self.last_capture_ns = None
        self.last_pose_ns = None
        self.last_invalid_ns = None
        self.resume_gaps = []
        self.intervals, self.ages = [], []
        self.poses = Counter()
        self.write_mtus = set()
        self.errors = []
        self.advertising = {"mode": "dbus"}
        self.started_ns = time.monotonic_ns()

    def bind_peer(self, peer):
        if peer is None:
            return
        if self.peer is None:
            self.peer = peer
            self.counts["CONNECT"] += 1
        elif self.peer != peer:
            raise ValueError("Temporary probe allows one peer at a time")

    def disconnect(self, peer):
        if peer != self.peer:
            return
        self.counts["DISCONNECT"] += 1
        self.peer = None
        previous_epoch = self.epoch
        while self.epoch == previous_epoch:
            self.epoch = secrets.randbits(64) or 1
        self.epoch_history.append(self.epoch)
        self.sequence.reset()
        self.last_capture_ns = None
        self.last_pose_ns = None
        self.last_invalid_ns = None

    def reject(self, message):
        self.counts["ERROR"] += 1
        if len(self.errors) < 40:
            self.errors.append(str(message))
        # Match the real receiver: a rejected packet does not replace the
        # previous observation. Its normal timestamp expiration still applies.
        raise DBusError("org.bluez.Error.Failed", str(message))

    def read_clock(self, sampled_pi_ns, peer):
        try:
            self.bind_peer(peer)
            self.counts["CLOCK_READ"] += 1
            return pack_clock(self.epoch, sampled_pi_ns)
        except (ValueError, TypeError) as exc:
            self.reject(exc)

    def receive(self, raw, received_ns, peer):
        try:
            self.bind_peer(peer)
            packet = decode_pose(raw)
            if packet.vehicle_id != 1:
                raise ValueError("Probe accepts vehicle ID1 only")
            if packet.epoch != self.epoch:
                raise ValueError("Wrong epoch; read CLOCK after connect/reconnect")
            if not is_fresh_capture(packet.capture_pi_ns, received_ns):
                raise ValueError("Capture is zero, in the future, or at least 300 ms old")
            if self.last_capture_ns is not None and packet.capture_pi_ns <= self.last_capture_ns:
                raise ValueError("Capture timestamp did not advance")
            if self.sequence.last_sequence is not None and not is_newer_sequence(packet.sequence, self.sequence.last_sequence):
                raise ValueError("Duplicate or backwards sequence")
            pose = (packet.x_mm, packet.y_mm, packet.heading_cdeg)
            if packet.valid and pose not in KNOWN_POSES:
                raise ValueError(f"Unexpected synthetic test pose: {pose}")
            # State is only advanced after every check has passed.
            self.sequence.accept(packet.sequence)
            self.last_capture_ns = packet.capture_pi_ns
            if not packet.valid:
                self.counts["INVALID"] += 1
                self.last_pose_ns = None
                if self.last_invalid_ns is None:
                    self.last_invalid_ns = received_ns
                return
            self.counts["POSE"] += 1
            self.poses[str(pose)] += 1
            self.ages.append((received_ns - packet.capture_pi_ns) / 1e6)
            if self.last_invalid_ns is not None:
                self.resume_gaps.append({"until_next_pose_ms": (received_ns - self.last_invalid_ns) / 1e6})
                self.last_invalid_ns = None
            if self.last_pose_ns is not None:
                self.intervals.append((received_ns - self.last_pose_ns) / 1e6)
            self.last_pose_ns = received_ns
        except (ValueError, TypeError, KeyError) as exc:
            self.reject(exc)

    def report(self):
        return {
            "purpose": "TEMPORARY SYNTHETIC ACP1/ACT1 BLE PROBE; NOT CAMERA/GPS DATA",
            "protocol": "ACP1_40B_ACT1_20B",
            "elapsed_s": (time.monotonic_ns() - self.started_ns) / 1e9,
            "counts": dict(self.counts),
            "known_pose_counts": dict(self.poses),
            "epoch": self.epoch,
            "epoch_history": self.epoch_history,
            "pose_interval_ms_excluding_invalid_gap": distribution(self.intervals),
            "capture_age_ms": distribution(self.ages),
            "capture_age_meaning": "Pi receive time minus conservatively translated synthetic PC observation time; not optical latency",
            "pose_resume_after_invalid": self.resume_gaps,
            "observed_write_att_mtus": sorted(self.write_mtus),
            "advertising": self.advertising,
            "errors": self.errors,
        }


class Service(ServiceInterface):
    def __init__(self):
        super().__init__("org.bluez.GattService1")

    @dbus_property(access=PropertyAccess.READ)
    def UUID(self) -> 's': return SERVICE_UUID

    @dbus_property(access=PropertyAccess.READ)
    def Primary(self) -> 'b': return True

    @dbus_property(access=PropertyAccess.READ)
    def Includes(self) -> 'ao': return []


class Characteristic(ServiceInterface):
    def __init__(self, char_uuid, flags, state):
        super().__init__("org.bluez.GattCharacteristic1")
        self.char_uuid, self.flags, self.state = char_uuid, flags, state

    @dbus_property(access=PropertyAccess.READ)
    def UUID(self) -> 's': return self.char_uuid

    @dbus_property(access=PropertyAccess.READ)
    def Service(self) -> 'o': return SERVICE

    @dbus_property(access=PropertyAccess.READ)
    def Flags(self) -> 'as': return self.flags

    @method()
    def ReadValue(self, options: 'a{sv}') -> 'ay':
        sampled_pi_ns = time.monotonic_ns()
        if "read" not in self.flags:
            raise DBusError("org.bluez.Error.NotPermitted", "POSE is write-only")
        if option(options, "offset", 0) != 0:
            raise DBusError("org.bluez.Error.InvalidOffset", "ACT1 requires one complete CLOCK read")
        return self.state.read_clock(sampled_pi_ns, option(options, "device"))

    @method()
    def WriteValue(self, value: 'ay', options: 'a{sv}'):
        received_ns = time.monotonic_ns()
        if "write" not in self.flags:
            raise DBusError("org.bluez.Error.NotPermitted", "CLOCK is read-only")
        if len(value) != POSE_PACKET_SIZE or option(options, "offset", 0) != 0:
            raise DBusError("org.bluez.Error.InvalidValueLength", "Exactly one complete 40-byte ACP1 write is required")
        if option(options, "prepare-authorize", False) or option(options, "type", "request") != "request":
            raise DBusError("org.bluez.Error.NotSupported", "Only a normal write-with-response is supported; no prepare/reliable/command")
        mtu = option(options, "mtu")
        if mtu is not None:
            self.state.write_mtus.add(int(mtu))
            if mtu < MIN_ATT_MTU:
                raise DBusError("org.bluez.Error.InvalidValueLength", "ATT MTU must be at least 43")
        self.state.receive(bytes(value), received_ns, option(options, "device"))


class Application(ServiceInterface):
    def __init__(self):
        super().__init__("org.freedesktop.DBus.ObjectManager")

    @method()
    def GetManagedObjects(self) -> 'a{oa{sa{sv}}}':
        objects = {SERVICE: {"org.bluez.GattService1": {
            "UUID": Variant('s', SERVICE_UUID), "Primary": Variant('b', True), "Includes": Variant('ao', [])}}}
        for path, char_uuid, flags in ((POSE, POSE_CHARACTERISTIC_UUID, ['write']),
                                      (CLOCK, CLOCK_CHARACTERISTIC_UUID, ['read'])):
            objects[path] = {"org.bluez.GattCharacteristic1": {
                "UUID": Variant('s', char_uuid), "Service": Variant('o', SERVICE),
                "Flags": Variant('as', flags)}}
        return objects


class Advertisement(ServiceInterface):
    def __init__(self, name):
        super().__init__("org.bluez.LEAdvertisement1")
        self.local_name = name

    @dbus_property(access=PropertyAccess.READ)
    def Type(self) -> 's': return "peripheral"

    @dbus_property(access=PropertyAccess.READ)
    def ServiceUUIDs(self) -> 'as': return [SERVICE_UUID]

    @dbus_property(access=PropertyAccess.READ)
    def LocalName(self) -> 's': return self.local_name

    @method()
    def Release(self): pass


async def btmgmt_command(executable, adapter, *arguments):
    """Run the official local MGMT CLI, without a shell or settings changes."""
    process = await asyncio.create_subprocess_exec(
        executable, "-i", adapter, *arguments,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        env=dict(os.environ, LC_ALL="C"),
    )
    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(), 10)
    except BaseException:
        process.kill()
        await process.communicate()
        raise
    output = (stdout + stderr).decode("utf-8", errors="replace")
    output = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", output)
    if process.returncode:
        raise RuntimeError(f"btmgmt {' '.join(arguments[:1])} failed ({process.returncode}): {output.strip()}")
    return output


def read_advertising_features(adapter):
    """READ-ONLY MGMT 0x003d; unlike btmgmt advinfo, retain occupied IDs.

    Wire reference: bluez/doc/mgmt-protocol.rst, Read Advertising Features.
    This socket only sends a six-byte read command, never a settings mutation.
    """
    index = int(adapter[3:])
    if not 0 <= index < 0xFFFF:
        raise RuntimeError("Invalid controller index")
    header = struct.Struct("<HHH")
    fixed = struct.Struct("<IBBBB")
    opcode = 0x003D
    with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI) as channel:
        # CPython's HCI bind tuple does not expose hci_channel on this Pi's
        # Python build. Bind the documented Linux sockaddr_hci through libc.
        address = ctypes.create_string_buffer(struct.pack("=HHH", socket.AF_BLUETOOTH, 0xFFFF, 3))
        libc = ctypes.CDLL(None, use_errno=True)
        bind = libc.bind
        bind.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_uint)
        bind.restype = ctypes.c_int
        if bind(channel.fileno(), ctypes.byref(address), 6) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        channel.settimeout(3)
        channel.sendall(header.pack(opcode, index, 0))
        deadline = time.monotonic() + 3
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Timed out reading advertising instance IDs")
            channel.settimeout(remaining)
            raw = channel.recv(4096)
            if len(raw) < header.size:
                continue
            event, event_index, length = header.unpack_from(raw)
            body = raw[header.size:]
            if event_index != index or event not in (0x0001, 0x0002):
                continue
            if len(body) != length or len(body) < 3:
                raise RuntimeError("Malformed MGMT response")
            reply_opcode, status = struct.unpack_from("<HB", body)
            if reply_opcode != opcode:
                continue
            if status:
                raise RuntimeError(f"Read Advertising Features failed: MGMT 0x{status:02x}")
            if event != 0x0001:
                continue
            data = body[3:]
            if len(data) < fixed.size:
                raise RuntimeError("Short Read Advertising Features response")
            flags, max_adv, max_scan, capacity, count = fixed.unpack_from(data)
            instances = list(data[fixed.size:])
            if len(instances) != count or len(set(instances)) != count or any(not 1 <= item <= 254 for item in instances):
                raise RuntimeError("Invalid advertising instance list")
            return {"supported_flags": flags, "max_adv_data_len": max_adv,
                    "max_scan_rsp_len": max_scan, "max_instances": capacity,
                    "instance_ids": instances}


async def start_legacy_advertisement(args, state):
    """Explicit workaround for kernel 6.18.34's extended-advertising bug.

    BlueZ 5.82 client/mgmt.c exposes add-adv -c -g -u -s -t and rm-adv.
    A read-only MGMT query gets occupied IDs so this explicit instance can be
    checked as unused. No existing instance is removed or overwritten.
    """
    executable = shutil.which("btmgmt")
    if executable is None:
        raise RuntimeError("--legacy-advertising requires installed btmgmt")
    features = await asyncio.to_thread(read_advertising_features, args.adapter)
    instance = args.legacy_instance
    if instance in features["instance_ids"]:
        raise RuntimeError(f"Advertising instance {instance} is already present; probe will not overwrite it")
    if len(features["instance_ids"]) >= features["max_instances"]:
        raise RuntimeError("Controller has no free advertising instance capacity")
    name = args.name.encode("ascii")
    scan_response = bytes((len(name) + 1, 0x09)) + name
    if len(scan_response) > 31:
        raise RuntimeError("Probe scan response exceeds the 31-byte legacy limit")
    timeout_seconds = math.ceil(args.seconds) + 30
    state.advertising = {
        "mode": "explicit_legacy_btmgmt", "instance": instance,
        "initial_advertising_features": features, "kernel_timeout_s": timeout_seconds,
        "scan_response_hex": scan_response.hex(),
        "source": "https://github.com/bluez/bluez/blob/5.82/client/mgmt.c",
    }
    output = await btmgmt_command(
        executable, args.adapter, "add-adv", "-c", "-g", "-u", SERVICE_UUID,
        "-s", scan_response.hex(), "-t", str(timeout_seconds), str(instance),
    )
    if re.search(rf"Instance added:\s*{instance}\b", output) is None:
        raise RuntimeError(f"Could not confirm owned advertising instance; bounded kernel timeout remains: {output.strip()}")
    state.advertising["registration_output"] = output.strip()
    return executable, instance


async def run(args, state):
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    adapter = f"/org/bluez/{args.adapter}"
    app_registered = adv_registered = False
    owned_legacy = None
    gatt = advertiser = None
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    def properties_changed(message):
        if (message.message_type == MessageType.SIGNAL and
                message.interface == "org.freedesktop.DBus.Properties" and
                message.member == "PropertiesChanged" and len(message.body) == 3):
            interface, changed, _ = message.body
            if interface == "org.bluez.Device1" and "Connected" in changed and not changed["Connected"].value:
                state.disconnect(message.path)

    try:
        rule = "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='org.bluez.Device1'"
        bus.add_message_handler(properties_changed)
        reply = await bus.call(Message(destination="org.freedesktop.DBus", path="/org/freedesktop/DBus",
                                       interface="org.freedesktop.DBus", member="AddMatch",
                                       signature="s", body=[rule]))
        if reply.message_type == MessageType.ERROR:
            raise RuntimeError(f"Cannot monitor disconnects: {reply.body}")
        intro = await bus.introspect("org.bluez", adapter)
        proxy = bus.get_proxy_object("org.bluez", adapter, intro)
        gatt = proxy.get_interface("org.bluez.GattManager1")
        advertiser = proxy.get_interface("org.bluez.LEAdvertisingManager1")
        for path, obj in ((ROOT, Application()), (SERVICE, Service()),
                          (POSE, Characteristic(POSE_CHARACTERISTIC_UUID, ['write'], state)),
                          (CLOCK, Characteristic(CLOCK_CHARACTERISTIC_UUID, ['read'], state)),
                          (ADV, Advertisement(args.name))):
            bus.export(path, obj)
        await gatt.call_register_application(ROOT, {})
        app_registered = True
        if args.legacy_advertising:
            owned_legacy = await start_legacy_advertisement(args, state)
        else:
            await advertiser.call_register_advertisement(ADV, {})
            adv_registered = True
        print(f"READY {args.name}: ACP1/ACT1 synthetic-only probe; no IPC or vehicle control", flush=True)
        try:
            await asyncio.wait_for(stop.wait(), args.seconds)
        except asyncio.TimeoutError:
            pass
    finally:
        if owned_legacy is not None:
            executable, instance = owned_legacy
            try:
                output = await btmgmt_command(executable, args.adapter, "rm-adv", str(instance))
                state.advertising["cleanup_output"] = output.strip()
            except Exception as exc:
                state.errors.append(f"Cleanup of owned legacy advertising instance {instance}: {exc}")
        for registered, manager, method_name, path in (
                (adv_registered, advertiser, "call_unregister_advertisement", ADV),
                (app_registered, gatt, "call_unregister_application", ROOT)):
            if registered:
                try:
                    await asyncio.wait_for(getattr(manager, method_name)(path), 3)
                except Exception as exc:
                    state.errors.append(f"Cleanup: {exc}")
        bus.disconnect()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="ACPTEST1")
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--seconds", type=float, default=45)
    parser.add_argument("--output", default="pi_probe_result.json")
    parser.add_argument("--legacy-advertising", action="store_true",
                        help="Explicit temporary btmgmt workaround; requires privileges and an unused instance ID")
    parser.add_argument("--legacy-instance", type=int, default=5,
                        help="Owned temporary instance ID used only with --legacy-advertising (default 5)")
    args = parser.parse_args()
    if not args.name.startswith("ACPTEST") or len(args.name) != 8 or args.name[-1] not in "123456789":
        parser.error("Only an ACPTEST1..9 temporary receiver name is allowed")
    if not math.isfinite(args.seconds) or not 1 <= args.seconds <= 600:
        parser.error("--seconds must be between 1 and 600")
    if not args.adapter.startswith("hci") or not args.adapter[3:].isdigit():
        parser.error("--adapter must be hci0, hci1, etc.")
    if not 1 <= args.legacy_instance <= 254:
        parser.error("--legacy-instance must be in 1..254; zero/all-instances is forbidden")
    state = ProbeState()
    result_code = 0
    try:
        asyncio.run(run(args, state))
    except Exception as exc:
        state.errors.append(f"Startup/runtime: {type(exc).__name__}: {exc}")
        result_code = 1
    report = state.report()
    Path(args.output).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    return result_code


if __name__ == "__main__":
    raise SystemExit(main())
