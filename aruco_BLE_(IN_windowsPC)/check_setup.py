"""Read-only dependency/configuration check: no camera and no radio connection."""
from pathlib import Path
import importlib
import importlib.metadata
import json
import sys

BASE = Path(__file__).resolve().parent
if (BASE/'vendor').is_dir():
    sys.path.insert(0, str(BASE/'vendor'))


def main():
    checks = []
    print('Python:', sys.version.split()[0], sys.executable)
    for name in ('cv2', 'numpy', 'bleak'):
        try:
            module = importlib.import_module(name)
            version = getattr(module, '__version__', None)
            if version is None:
                version = importlib.metadata.version(name)
            if name == 'cv2' and not hasattr(module, 'aruco'):
                raise RuntimeError('OpenCV does not contain the ArUco module')
            print(f'OK {name}: {version}')
            checks.append(True)
        except Exception as error:
            print(f'FAIL {name}: {error}')
            checks.append(False)
    for name in ('tracking_config.json', 'ble_config.json'):
        try:
            json.loads((BASE/name).read_text(encoding='utf-8-sig'))
            print('OK configuration:', name)
            checks.append(True)
        except Exception as error:
            print('FAIL configuration:', name, error)
            checks.append(False)
    try:
        from aruco_ble_sender import load_config
        from tracking_ble_bridge import load_mounts
        load_mounts(load_config(BASE/'ble_config.json'))
        print('OK BLE target mapping and mounting configuration')
        checks.append(True)
    except Exception as error:
        print('FAIL BLE target mapping:', error)
        checks.append(False)
    try:
        import numpy as np
        with np.load(BASE/'camera_calibration_4k.npz', allow_pickle=False) as data:
            assert data['camera_matrix'].shape == (3,3)
        print('OK camera calibration file')
        checks.append(True)
    except Exception as error:
        print('FAIL camera calibration:', error)
        checks.append(False)
    print('This check does not prove a Pi BLE receiver exists or that 5 radio links work.')
    return 0 if all(checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
