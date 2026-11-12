"""Exercise the camera-to-BLE entry point without opening camera/radio/UI."""
from pathlib import Path
import time
import unittest
from unittest.mock import patch
import numpy as np
import track_aruco_4k as tracker
from test_tracking_offline import observations


class MainBLETest(unittest.TestCase):
    def test_live_frame_and_camera_gap_reach_sender_with_cleanup(self):
        sent = []
        events = []
        source = observations()
        corners = [v.reshape(1,4,2) for v in source.values()]
        ids = np.array(list(source.keys()), dtype=np.int32).reshape(-1,1)
        class Detector:
            def detectMarkers(self, gray): return corners, ids, []
        class Camera:
            def __init__(self, index): self.n = 0
            def get(self, after):
                self.n += 1
                if self.n == 2: return None
                return self.n, time.monotonic(), time.time(), np.zeros((720,1280,3), dtype=np.uint8)
            def close(self): events.append('camera_closed')
        class Sender:
            def __init__(self, config): pass
            def start(self): events.append('start')
            def stop(self): events.append('stop')
            def publish_frame(self, *args, **kwargs): sent.append((args, kwargs))
            def invalidate_all(self, reason): events.append(reason)
            def snapshot(self): return {'cars':{},'fatal_error':''}
        import aruco_ble_sender
        with (patch.object(tracker, 'LatestFrameCamera', Camera),
              patch.object(tracker, 'make_detector', Detector),
              patch.object(aruco_ble_sender, 'BLEPoseSender', Sender),
              patch.object(tracker.cv2, 'namedWindow'), patch.object(tracker.cv2, 'resizeWindow'),
              patch.object(tracker.cv2, 'imshow'), patch.object(tracker.cv2, 'destroyAllWindows'),
              patch.object(tracker.cv2, 'waitKey', side_effect=[-1,ord('q')])):
            tracker.main(['--ble','--ble-config',str(Path(__file__).with_name('ble_config.json'))])
        self.assertEqual(len(sent), 1)
        payload = sent[0][0][2]
        self.assertTrue(payload)
        self.assertIn('heading_deg', next(iter(payload.values())))
        self.assertNotIn('yaw_deg', next(iter(payload.values())))
        self.assertIn('camera_unavailable', events)
        self.assertIn('camera_closed', events)
        self.assertIn('stop', events)


if __name__ == '__main__':
    unittest.main()
