"""BLE connection and service metadata only: no camera or characteristic data."""
import argparse
import asyncio
import json
from pathlib import Path
import re
import sys

SERVICE = "7b9c0001-6a54-4f1b-9f65-2c7b8d1e4a90"
POSE = "7b9c0002-6a54-4f1b-9f65-2c7b8d1e4a90"
CLOCK = "7b9c0003-6a54-4f1b-9f65-2c7b8d1e4a90"


async def check(args, report):
    sys.path.insert(0, str(Path(__file__).with_name("vendor")))
    from bleak import BleakClient, BleakScanner
    print(f"Scanning 10 seconds for exactly {args.name} / {args.address}", flush=True)
    found = await BleakScanner.discover(timeout=10.0, return_adv=True)
    matches = [device for device, adv in found.values()
               if str(device.address).upper() == args.address
               and (adv.local_name or device.name or "") == args.name]
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one matching device; found {len(matches)}")
    client = BleakClient(matches[0], winrt={"use_cached_services": False})
    try:
        print("Connecting (timeout 30 seconds)...", flush=True)
        await asyncio.wait_for(client.connect(), timeout=30.0)
        report["connected"] = bool(client.is_connected)
        if not client.is_connected:
            raise RuntimeError("Connect returned without an active connection")
        report["mtu"] = int(client.mtu_size)
        service = client.services.get_service(SERVICE)
        characteristics = {} if service is None else {
            char.uuid.lower(): list(char.properties) for char in service.characteristics}
        report.update(service_uuid=SERVICE, service_present=service is not None,
                      pose_present=POSE in characteristics, clock_present=CLOCK in characteristics,
                      characteristic_properties=characteristics)
        print(f"Connected. MTU={report['mtu']}; service={report['service_present']}; "
              f"POSE={report['pose_present']}; CLOCK={report['clock_present']}", flush=True)
        if not all(report[key] for key in ("service_present", "pose_present", "clock_present")):
            raise RuntimeError("Connected, but expected service/characteristic metadata is missing")
        print("Holding connection for 3 seconds. No characteristic data operations.", flush=True)
        await asyncio.sleep(3.0)
        report["held_connected_3_seconds"] = bool(client.is_connected)
        if not client.is_connected:
            raise RuntimeError("Connection dropped during the 3-second hold")
    finally:
        print("Disconnecting...", flush=True)
        await asyncio.wait_for(client.disconnect(), timeout=5.0)
        report["disconnected"] = not bool(client.is_connected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="ACPTEST1")
    parser.add_argument("--address", help="Exact Pi Bluetooth address; required for ACE targets")
    parser.add_argument("--output", default=str(Path(__file__).with_name("ble_connection_result.json")))
    args = parser.parse_args()
    if args.name != "ACPTEST1" and not re.fullmatch(r"ACE[1-9][0-9]{0,4}", args.name):
        parser.error("Use ACPTEST1 or an exact configured ACE vehicle name")
    if args.address is None:
        if args.name != "ACPTEST1":
            parser.error("Supply --address explicitly when selecting an ACE receiver")
        args.address = "88:A2:9E:9F:DA:8E"
    args.address = args.address.upper()
    if not re.fullmatch(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", args.address):
        parser.error("--address must be an exact AA:BB:CC:DD:EE:FF address")
    report = {"purpose": "BLE CONNECTION AND METADATA ONLY", "name": args.name,
              "address": args.address, "connected": False,
              "characteristic_data_reads": 0, "characteristic_data_writes": 0,
              "notifications_subscribed": 0, "camera_opened": False}
    code = 0
    try:
        asyncio.run(check(args, report))
    except (Exception, KeyboardInterrupt) as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        code = 1
    Path(args.output).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
