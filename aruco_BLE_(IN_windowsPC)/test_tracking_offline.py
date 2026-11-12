"""Offline numerical regressions. Never opens a camera or changes calibration."""
from pathlib import Path
import json
import unittest
import threading
from types import SimpleNamespace
from unittest.mock import Mock, patch
import track_aruco_4k as tracker
import cv2
import numpy as np
from track_aruco_4k import (AdaptivePoseFilter, Pose, TrackingEngine, get_marker_obj_points,
    interpolate_rotation, reference_world_points, reprojection_rms,
    rotation_distance_deg, unique_observations)

BASE=Path(__file__).resolve().parent
CONFIG=json.loads((BASE/'tracking_config.json').read_text(encoding='utf8'))
with np.load(BASE/'camera_calibration_4k.npz') as c:
    K,D=c['camera_matrix'].copy(),c['dist_coeffs'].copy()

def Rz(degrees):
    return cv2.Rodrigues(np.array([0.,0.,np.deg2rad(degrees)]))[0]

CAM_R=cv2.Rodrigues(np.array([np.pi-.08, .10, .02]))[0]
CAM_T=-CAM_R@np.array([[.91],[.48],[1.5]])

def observations(position=(.60,.40,.08),yaw=22.,refs=(6,7),noise=0.,rng=None):
    result={}
    for mid,pts in reference_world_points(CONFIG).items():
        if mid in refs:
            result[mid]=cv2.projectPoints(pts,cv2.Rodrigues(CAM_R)[0],CAM_T,K,D)[0].reshape(4,2)
    if position is not None:
        pts=get_marker_obj_points()@Rz(yaw).T+np.asarray(position)
        result[1]=cv2.projectPoints(pts,cv2.Rodrigues(CAM_R)[0],CAM_T,K,D)[0].reshape(4,2)
    if noise:
        result={i:c+rng.normal(0,noise,c.shape) for i,c in result.items()}
    return result

class TrackingTests(unittest.TestCase):
    def test_rotation_pi_boundary(self):
        a=cv2.Rodrigues(np.array([np.deg2rad(179),0.,0.]))[0]
        b=cv2.Rodrigues(np.array([np.deg2rad(-179),0.,0.]))[0]
        got=interpolate_rotation(a,b,.35)
        self.assertAlmostEqual(rotation_distance_deg(got,b),1.3,places=7)
        np.testing.assert_allclose(got.T@got,np.eye(3),atol=1e-12)

    def test_filter_reacquire_is_fresh(self):
        f=AdaptivePoseFilter()
        a=Pose(np.eye(3),np.zeros((3,1)))
        b=Pose(Rz(110),np.ones((3,1)))
        f.update(a,1.,1)
        got=f.update(b,1.1,3)
        np.testing.assert_allclose(got.t,b.t)
        np.testing.assert_allclose(got.R,b.R)

    def test_filter_gap_is_fresh(self):
        f=AdaptivePoseFilter(max_gap_seconds=.2)
        f.update(Pose(np.eye(3),np.zeros((3,1))),1.,1)
        target=Pose(Rz(-50),np.ones((3,1)))
        got=f.update(target,1.3,2)
        np.testing.assert_allclose(got.t,target.t)

    def test_bottom_left_corner_coordinate(self):
        refs=reference_world_points(CONFIG)
        np.testing.assert_allclose(refs[6].mean(axis=0),[.046,.050,0])
        np.testing.assert_allclose(refs[7].mean(axis=0),[1.770,.050,0])
        for points in refs.values():
            self.assertAlmostEqual(np.linalg.norm(points[1]-points[0]),.048)

    def test_noiseless_dual_pose(self):
        engine=TrackingEngine(K,D,CONFIG)
        result=engine.process(observations(),1.)
        self.assertEqual(result['status'],'DUAL REF 06+07')
        vehicle=result['vehicles'][1]
        np.testing.assert_allclose(vehicle['xyz_mm'],[600,400,80],atol=.01)
        self.assertAlmostEqual(vehicle['yaw_deg'],22.,places=4)

    def test_each_single_reference_has_same_world_origin(self):
        for refs in [(6,),(7,)]:
            engine=TrackingEngine(K,D,CONFIG)
            result=engine.process(observations(refs=refs),1.)
            self.assertIn(1,result['vehicles'])
            np.testing.assert_allclose(result['vehicles'][1]['xyz_mm'],[600,400,80],atol=.05)

    def test_lost_vehicle_removes_old_state(self):
        engine=TrackingEngine(K,D,CONFIG)
        engine.process(observations(),1.)
        missed=engine.process(observations(position=None),1.03)
        self.assertNotIn(1,missed['vehicles'])
        self.assertNotIn(1,engine.vehicle_filters)
        self.assertNotIn(1,engine.vehicle_camera_previous)
        got=engine.process(observations(position=(1.,.7,.08),yaw=-135),1.06)
        np.testing.assert_allclose(got['vehicles'][1]['xyz_mm'],[1000,700,80],atol=.05)
        self.assertAlmostEqual(got['vehicles'][1]['yaw_deg'],-135,places=4)

    def test_lost_reference_never_holds_world_output(self):
        engine=TrackingEngine(K,D,CONFIG)
        engine.process(observations(),1.)
        got=engine.process(observations(refs=()),1.03)
        self.assertIsNone(got['camera'])
        self.assertEqual(got['vehicles'],{})
        self.assertEqual(engine.vehicle_filters,{})
        self.assertIsNone(engine.camera_previous)

    def test_bad_secondary_reference_keeps_current_primary(self):
        engine=TrackingEngine(K,D,CONFIG)
        data=observations()
        data[7][0]+=[150,-100]
        got=engine.process(data,1.)
        self.assertEqual(got['references'],[6])
        self.assertEqual(got['status'],'SINGLE REF 06')
        np.testing.assert_allclose(got['vehicles'][1]['xyz_mm'],[600,400,80],atol=.05)

    def test_secondary_correction_checks_each_marker_error(self):
        data=observations()
        center=data[7].mean(axis=0)
        angle=np.deg2rad(10.)
        rotation=np.array([[np.cos(angle),-np.sin(angle)],
                           [np.sin(angle),np.cos(angle)]])
        data[7]=(data[7]-center)@rotation.T+center
        # This rotated square is individually plausible, but contradicts ID06.
        secondary=TrackingEngine(K,D,CONFIG).process({7:data[7]},1.)
        self.assertIsNotNone(secondary['camera'])
        got=TrackingEngine(K,D,CONFIG).process(data,1.)
        self.assertEqual(got['references'],[6])
        np.testing.assert_allclose(got['vehicles'][1]['xyz_mm'],[600,400,80],atol=.05)

    def test_bad_primary_reference_uses_current_secondary(self):
        data=observations()
        data[6][0]+=[150,-100]
        got=TrackingEngine(K,D,CONFIG).process(data,1.)
        self.assertEqual(got['references'],[7])
        self.assertEqual(got['status'],'SINGLE REF 07')
        np.testing.assert_allclose(got['vehicles'][1]['xyz_mm'],[600,400,80],atol=.05)

    def test_both_bad_references_clear_old_world(self):
        engine=TrackingEngine(K,D,CONFIG)
        engine.process(observations(),1.)
        data=observations()
        for marker_id in (6,7):
            data[marker_id][0]+=[150,-100]
        got=engine.process(data,1.03)
        self.assertIsNone(got['camera'])
        self.assertEqual(got['vehicles'],{})
        self.assertIsNone(engine.camera_previous)

    def test_failed_correction_uses_moved_camera_current_frame(self):
        engine=TrackingEngine(K,D,CONFIG)
        engine.process(observations(),1.)
        camera_t=CAM_T+np.array([[.10],[.04],[.03]])
        data={i:cv2.projectPoints(points,cv2.Rodrigues(CAM_R)[0],camera_t,K,D)[0].reshape(4,2)
              for i,points in reference_world_points(CONFIG).items()}
        points=get_marker_obj_points()@Rz(22.).T+np.array([.6,.4,.08])
        data[1]=cv2.projectPoints(points,cv2.Rodrigues(CAM_R)[0],camera_t,K,D)[0].reshape(4,2)
        data[7][0]+=[150,-100]
        got=engine.process(data,1.03)
        self.assertEqual(got['references'],[6])
        self.assertTrue(got['camera_changed'])
        np.testing.assert_allclose(got['camera'].t,camera_t,atol=1e-6)
        np.testing.assert_allclose(got['vehicles'][1]['xyz_mm'],[600,400,80],atol=.05)

    def test_reference_tolerance_does_not_relax_vehicle(self):
        data=observations(refs=(6,))
        data[6][0]+=[8,-8]
        got=TrackingEngine(K,D,CONFIG).process(data,1.)
        self.assertIsNotNone(got['camera'])
        self.assertGreater(got['reference_rms_px'],2.)
        self.assertLess(got['reference_rms_px'],6.)
        self.assertIn(1,got['vehicles'])
        strict=dict(CONFIG,reference_max_reprojection_rms_px=2.)
        self.assertIsNone(TrackingEngine(K,D,strict).process(data,1.)['camera'])
        data=observations()
        data[1][0]+=[20,-20]
        got=TrackingEngine(K,D,CONFIG).process(data,1.)
        self.assertIsNotNone(got['camera'])
        self.assertNotIn(1,got['vehicles'])

    def test_duplicate_ids_are_ignored(self):
        c=np.zeros((1,4,2))
        got=unique_observations([c,c,c],np.array([[6],[6],[7]]))
        self.assertNotIn(6,got)
        self.assertIn(7,got)

    def test_rms_definition(self):
        obj=get_marker_obj_points()
        r=np.array([.3,.1,.2]);t=np.array([0.,0.,1.5])
        img=cv2.projectPoints(obj,r,t,K,D)[0].reshape(4,2)
        img+=np.array([3.,4.])
        self.assertAlmostEqual(reprojection_rms(obj,img,r,t,K,D),5.)

class CameraStartupTests(unittest.TestCase):
    def make_camera(self):
        # Run the real worker synchronously so frame delivery is deterministic.
        camera = tracker.LatestFrameCamera.__new__(tracker.LatestFrameCamera)
        camera.index = 1
        camera.condition = threading.Condition()
        camera.stop = threading.Event()
        camera.latest = None
        camera.error = None
        return camera

    def test_4k_first_frame_is_delivered_and_next_frame_has_no_format_check(self):
        camera = self.make_camera()
        first = SimpleNamespace(shape=(2160, 3840, 3))
        second = object()  # Accessing .shape here would fail the capture loop.
        capture = Mock()
        capture.isOpened.return_value = True
        delivered = []

        def read():
            if not delivered:
                delivered.append(None)
                return True, first
            delivered.append(camera.get(0))
            camera.stop.set()
            return True, second

        capture.read.side_effect = read
        with patch.object(tracker.cv2, 'VideoCapture', return_value=capture):
            camera._run()

        self.assertIsNone(camera.error)
        self.assertEqual(delivered[1][0], 1)
        self.assertIs(delivered[1][3], first)
        self.assertEqual(camera.latest[0], 2)
        self.assertIs(camera.latest[3], second)
        capture.release.assert_called_once()

    def test_negotiated_non_4k_frame_is_rejected_before_delivery(self):
        camera = self.make_camera()
        capture = Mock()
        capture.isOpened.return_value = True
        capture.set.return_value = True
        capture.get.return_value = 3840  # Only the actual frame is authoritative.
        capture.read.return_value = (True, SimpleNamespace(shape=(1080, 1920, 3)))
        with patch.object(tracker.cv2, 'VideoCapture', return_value=capture):
            camera._run()

        self.assertIsNone(camera.latest)
        with self.assertRaisesRegex(RuntimeError, '1920x1080.*3840x2160'):
            camera.get(0)
        capture.read.assert_called_once()
        capture.release.assert_called_once()

    def test_failed_initial_read_is_reported_and_releases_camera(self):
        camera = self.make_camera()
        capture = Mock()
        capture.isOpened.return_value = True
        capture.read.return_value = (False, None)
        with patch.object(tracker.cv2, 'VideoCapture', return_value=capture):
            camera._run()

        self.assertIsNone(camera.latest)
        with self.assertRaisesRegex(RuntimeError, 'Camera frame read failed'):
            camera.get(0)
        capture.release.assert_called_once()

    def test_backend_fallback_also_checks_actual_startup_frame(self):
        camera = self.make_camera()
        unavailable = Mock()
        unavailable.isOpened.return_value = False
        fallback = Mock()
        fallback.isOpened.return_value = True
        fallback.read.return_value = (True, SimpleNamespace(shape=(720, 1280, 3)))
        with patch.object(tracker.cv2, 'VideoCapture', side_effect=[unavailable, fallback]):
            camera._run()

        self.assertIsNone(camera.latest)
        with self.assertRaisesRegex(RuntimeError, '1280x720.*3840x2160'):
            camera.get(0)
        unavailable.release.assert_called_once()
        fallback.release.assert_called_once()


def run_simulations():
    rng=np.random.default_rng(480607)
    engine=TrackingEngine(K,D,CONFIG)
    raw,smoothed,raw_yaw,filtered_yaw=[],[],[],[]
    for i in range(360):
        result=engine.process(observations(noise=.12,rng=rng),1+i/30)
        if 1 in result['vehicles'] and i>=60:
            v=result['vehicles'][1]
            raw.append(v['raw_world_pose'].t.ravel()*1000)
            smoothed.append(v['xyz_mm'])
            R=v['raw_world_pose'].R
            raw_yaw.append(np.degrees(np.arctan2(R[1,0],R[0,0])))
            filtered_yaw.append(v['yaw_deg'])
    raw,smoothed=np.array(raw),np.array(smoothed)
    metrics={'simulation_only':True,'corner_noise_std_px':.12,'simulated_fps':30,
        'static_samples':len(raw),'raw_position_std_mm':np.std(raw,axis=0).tolist(),
        'filtered_position_std_mm':np.std(smoothed,axis=0).tolist(),
        'raw_yaw_std_deg':float(np.std(raw_yaw)),
        'filtered_yaw_std_deg':float(np.std(filtered_yaw))}
    # Compare camera registration with identical noisy reference observations.
    rng=np.random.default_rng(6007)
    ref_errors={1:[],2:[]}
    for i in range(200):
        obs=observations(position=None,noise=.12,rng=rng)
        for count in [1,2]:
            eng=TrackingEngine(K,D,CONFIG)
            use=obs if count==2 else {6:obs[6]}
            result=eng.process(use,1.)
            cam=result['camera']
            if cam is not None:
                world=cam.R.T@(CAM_R@np.array([[1.2],[.8],[0.]])+CAM_T-cam.t)
                ref_errors[count].append((world.ravel()-[1.2,.8,0])*1000)
    for count in [1,2]:
        err=np.array(ref_errors[count])
        metrics[f'{count}_reference_world_xy_rmse_mm']=float(np.sqrt(np.mean(np.sum(err[:,:2]**2,axis=1))))
    # Constant-velocity segment: adaptive filtering should catch up while moving.
    eng=TrackingEngine(K,D,CONFIG);errors=[]
    for i in range(150):
        x=.3+.3*i/30
        got=eng.process(observations(position=(x,.4,.08),yaw=22),1+i/30)
        if 1 in got['vehicles'] and i>=30:
            errors.append(x*1000-got['vehicles'][1]['xyz_mm'][0])
    metrics['moving_300mm_s_mean_x_lag_mm']=float(np.mean(errors))
    assert np.all(np.std(smoothed,axis=0) < np.std(raw,axis=0)),metrics
    assert np.std(filtered_yaw)<np.std(raw_yaw),metrics
    assert metrics['2_reference_world_xy_rmse_mm'] < metrics['1_reference_world_xy_rmse_mm'],metrics
    (BASE/'수정후_수치시험.json').write_text(json.dumps(metrics,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(metrics,ensure_ascii=True,indent=2))

if __name__=='__main__':
    suite=unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case)
                             for case in (TrackingTests, CameraStartupTests))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():raise SystemExit(1)
    run_simulations()
