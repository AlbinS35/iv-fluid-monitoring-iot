"""
Unit and Integration Test Suite for IV Fluid Telematics System
Aligned with Amal Jyothi College of Engineering MCA 24MCAT213 Project Report
"""
import unittest
from app import app


class TestIVTelematicsReportCompliance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.client = app.test_client()

    def test_01_root_dashboard(self):
        """Verify dashboard HTML loads with report title and elements."""
        res = self.client.get('/')
        self.assertEqual(res.status_code, 200)
        self.assertIn(b'Smart IoT IV Fluid Infusion Monitoring', res.data)
        self.assertIn(b'WARD-A-BED-07', res.data)
        self.assertIn(b'customChartCanvas', res.data)

    def test_02_health_endpoint(self):
        """Verify health check returns valid JSON status (Report Table 9.1)."""
        res = self.client.get('/api/health')
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertEqual(data.get('status'), 'UP')
        self.assertIn('database_engine', data)

    def test_03_telemetry_ingest(self):
        """Verify ESP32 packet ingestion with POST /api/telemetry (Report Table 9.1)."""
        payload = {
            "device_id": "WARD-A-BED-07",
            "dpm": 40.0,
            "flow_ml_hr": 120.0,
            "vol_infused_ml": 128.0,
            "vol_remaining_ml": 372.0,
            "status": "NORMAL"
        }
        res = self.client.post('/api/telemetry', json=payload)
        self.assertEqual(res.status_code, 201)
        data = res.get_json()
        self.assertEqual(data.get('status'), 'success')
        self.assertEqual(data['data']['device_id'], 'WARD-A-BED-07')
        self.assertEqual(data['data']['dpm'], 40.0)
        self.assertEqual(data['data']['flow_ml_hr'], 120.0)
        self.assertEqual(data['data']['status'], 'NORMAL')

    def test_04_telemetry_latest_by_device(self):
        """Verify GET /api/latest/<device_id> (Report Table 9.1)."""
        res = self.client.get('/api/latest/WARD-A-BED-07')
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertEqual(data.get('status'), 'success')
        self.assertEqual(data['data']['device_id'], 'WARD-A-BED-07')

    def test_05_telemetry_history_by_device(self):
        """Verify GET /api/history/<device_id>?limit=30 (Report Table 9.1)."""
        res = self.client.get('/api/history/WARD-A-BED-07?limit=30')
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertEqual(data.get('status'), 'success')
        self.assertIsInstance(data.get('data'), list)
        self.assertGreaterEqual(len(data['data']), 1)

    def test_06_devices_list(self):
        """Verify GET /api/devices lists active beds (Report Table 9.1)."""
        res = self.client.get('/api/devices')
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertEqual(data.get('status'), 'success')
        self.assertIsInstance(data.get('devices'), list)

    def test_07_alert_conditions_normalization(self):
        """Verify Report Table 7.1 alert states: NO_FLOW, LOW_VOLUME, FREE_FLOW, RATE_ERR."""
        # Free flow (>150 DPM)
        r1 = self.client.post('/api/telemetry', json={
            "device_id": "WARD-A-BED-07",
            "dpm": 160.0,
            "vol_remaining_ml": 300.0
        })
        self.assertEqual(r1.status_code, 201)
        self.assertEqual(r1.get_json()['data']['status'], 'FREE_FLOW')

        # No flow / Blockage (0 DPM)
        r2 = self.client.post('/api/telemetry', json={
            "device_id": "WARD-A-BED-07",
            "dpm": 0.0,
            "vol_remaining_ml": 250.0
        })
        self.assertEqual(r2.status_code, 201)
        self.assertEqual(r2.get_json()['data']['status'], 'NO_FLOW')

        # Low volume (<= 50 mL / <= 10%)
        r3 = self.client.post('/api/telemetry', json={
            "device_id": "WARD-A-BED-07",
            "dpm": 40.0,
            "vol_remaining_ml": 35.0
        })
        self.assertEqual(r3.status_code, 201)
        self.assertEqual(r3.get_json()['data']['status'], 'LOW_VOLUME')

        # No flow / Blockage: Even if status is sent as 'NORMAL', 0 DPM must normalize to 'NO_FLOW'
        r4 = self.client.post('/api/telemetry', json={
            "device_id": "WARD-A-BED-07",
            "dpm": 0.0,
            "vol_remaining_ml": 400.0,
            "status": "NORMAL"
        })
        self.assertEqual(r4.status_code, 201)
        self.assertEqual(r4.get_json()['data']['status'], 'NO_FLOW')

    def test_08_container_volume_config(self):
        """Verify dynamic IV container volume setting (e.g. 200 mL, 150 mL)."""
        # Set container volume to 200 mL
        res = self.client.post('/api/container', json={
            "device_id": "WARD-A-BED-07",
            "container_vol_ml": 200.0
        })
        self.assertEqual(res.status_code, 200)
        data = res.get_json()
        self.assertEqual(data.get('container_vol_ml'), 200.0)

        # Ingest reading for 200 mL bottle with 180 mL remaining
        res_ingest = self.client.post('/api/telemetry', json={
            "device_id": "WARD-A-BED-07",
            "dpm": 40.0,
            "vol_remaining_ml": 180.0
        })
        self.assertEqual(res_ingest.status_code, 201)
        ingest_data = res_ingest.get_json()['data']
        self.assertEqual(ingest_data['container_vol_ml'], 200.0)
        self.assertEqual(ingest_data['vol_infused_ml'], 20.0)

        # Verify GET /api/latest reflects the 200 mL container size
        res_latest = self.client.get('/api/latest/WARD-A-BED-07')
        self.assertEqual(res_latest.status_code, 200)
        latest_data = res_latest.get_json()['data']
        self.assertEqual(latest_data['container_vol_ml'], 200.0)
        self.assertEqual(latest_data['vol_remaining_ml'], 180.0)


if __name__ == '__main__':
    unittest.main()
