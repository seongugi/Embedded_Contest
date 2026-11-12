"""Read the local Windows adapter address for the Pi receiver --peer argument."""
import asyncio
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).with_name('vendor')))
from winrt.windows.devices.bluetooth import BluetoothAdapter


async def main():
    adapter = await BluetoothAdapter.get_default_async()
    if adapter is None:
        raise RuntimeError('No default Windows Bluetooth adapter')
    address = ':'.join(f'{(adapter.bluetooth_address >> shift) & 255:02X}'
                       for shift in range(40, -1, -8))
    print('Windows Bluetooth address:', address)
    print('Pi receiver argument: --peer', address)
    print('Pairing and trust on the Pi are still required.')


if __name__ == '__main__':
    asyncio.run(main())
