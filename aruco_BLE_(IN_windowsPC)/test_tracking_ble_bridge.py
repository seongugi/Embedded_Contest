import math
from types import SimpleNamespace
import unittest
import numpy as np
from tracking_ble_bridge import publish_tracking_result, load_mounts


def item(xyz=(123.25, 999.5, 71), yaw=0, R=None):
    a = math.radians(yaw)
    if R is None:
        R = np.array([[math.cos(a), -math.sin(a), 0],
                      [math.sin(a), math.cos(a), 0], [0, 0, 1]])
    return {'xyz_mm': xyz, 'pose': SimpleNamespace(R=R)}


class FakeSender:
    def publish_frame(self, *args, **kwargs):
        self.last = args, kwargs


class BridgeTests(unittest.TestCase):
    def test_only_current_vehicle_poses_routed_in_mm(self):
        s = FakeSender()
        result = {'camera': object(), 'vehicles': {1: item(), 3: item(yaw=90), 6: item()}}
        got = publish_tracking_result(s, result, 123, 456_000_000)
        self.assertEqual(set(got), {1, 3})
        self.assertEqual(got[1], {'x_mm':123.25,'y_mm':999.5,'heading_deg':90})
        self.assertAlmostEqual(got[3]['heading_deg'], 0)
        self.assertEqual(s.last[0][:2], (123, 456_000_000))

    def test_heading_cardinal_axes_match_receiver(self):
        for yaw, expected in [(0,90), (90,0), (-90,180), (180,270)]:
            got = publish_tracking_result(FakeSender(), {'camera':1, 'vehicles':{1:item(yaw=yaw)}}, 1, 1)
            self.assertAlmostEqual(got[1]['heading_deg'], expected)

    def test_mount_offset_uses_measured_3d_orientation(self):
        R = np.array([[1,0,0], [0,.5,-math.sqrt(.75)], [0,math.sqrt(.75),.5]])
        mounts = load_mounts({'cars':[{'marker_id':1, 'heading_offset_deg':45,
                                      'target_offset_marker_mm':[0,0,-10]}]})
        got = publish_tracking_result(FakeSender(), {'camera':1,
            'vehicles':{1:item(xyz=(10,20,30), R=R)}}, 1, 1, mounts)
        self.assertAlmostEqual(got[1]['heading_deg'], math.degrees(math.atan2(1,.5)))
        self.assertAlmostEqual(got[1]['x_mm'], 10)
        self.assertAlmostEqual(got[1]['y_mm'], 20 + 10*math.sqrt(.75))

    def test_invalid_mount_rejected_at_start(self):
        for offset in ([1,2], [1,2,float('nan')]):
            with self.assertRaises(ValueError):
                load_mounts({'cars':[{'marker_id':1,'target_offset_marker_mm':offset}]})

    def test_out_of_plane_forward_axis_projects_to_receiver_heading(self):
        # Marker +Z points east and +Y north after a 90-degree rotation
        # about world Y. An optical axis equally along local +Y/+Z must
        # therefore point northeast, regardless of the supplied vector length.
        R = np.array([[0,0,1], [0,1,0], [-1,0,0]])
        for magnitude in (4, 1e308, 1e-300):
            mounts = load_mounts({'cars':[{'marker_id':1,
                'heading_offset_deg':180,
                'forward_axis_marker':[0,magnitude,magnitude]}]})
            self.assertAlmostEqual(float(np.linalg.norm(mounts[1][0])), 1)
            got = publish_tracking_result(FakeSender(), {'camera':1,
                'vehicles':{1:item(xyz=(10,20,30), R=R)}}, 1, 1, mounts)
            self.assertEqual(got[1]['x_mm'], 10)
            self.assertEqual(got[1]['y_mm'], 20)
            self.assertAlmostEqual(got[1]['heading_deg'], 45)

    def test_invalid_forward_axis_rejected_at_start(self):
        for axis in ([0,0,0], [1,2], [0,0,float('nan')],
                     [0,0,float('inf')], [[1,2,3]], None):
            with self.subTest(axis=axis), self.assertRaises(ValueError):
                load_mounts({'cars':[{'marker_id':1,'forward_axis_marker':axis}]})

    def test_lost_marker_does_not_republish_previous_pose(self):
        s = FakeSender()
        publish_tracking_result(s, {'camera':1,'vehicles':{1:item()}}, 1, 1)
        self.assertEqual(publish_tracking_result(s, {'camera':1,'vehicles':{}}, 2, 2), {})
        self.assertEqual(s.last[1]['reason'], 'not_detected')

    def test_missing_world_reference_never_sends_camera_coordinates(self):
        s = FakeSender()
        self.assertEqual(publish_tracking_result(s, {'camera':None,'vehicles':{1:item()}}, 1, 1), {})
        self.assertEqual(s.last[1]['reason'],'no_reference')

    def test_nonfinite_or_vertical_heading_never_published(self):
        vertical = np.array([[0,0,-1], [0,1,0], [1,0,0]])
        for invalid in (item(xyz=(float('nan'),2,3)), item(R=vertical)):
            self.assertEqual(publish_tracking_result(FakeSender(),
                             {'camera':1,'vehicles':{1:invalid}}, 1, 1), {})


if __name__ == '__main__':
    unittest.main()
