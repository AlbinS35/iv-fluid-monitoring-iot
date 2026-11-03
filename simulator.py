"""
========================================================================================
Project: Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
Institution: Amal Jyothi College of Engineering | MCA 24MCAT213
Component: Hardware Edge Node Simulator
Description:
    Simulates real-world ESP32 + TCRT5000 IR drop sensor telemetry stream.
    Posts to Ward Gateway REST endpoint /api/telemetry every 5 seconds.
    Accurately simulates Romsons Macro Set (20 drops = 1 mL) and Table 7.1 alerts.
========================================================================================
"""

import time
import random
import argparse
import requests

API_URL = "http://localhost:5000/api/telemetry"
DEVICE_ID = "WARD-A-BED-07"
DRIP_FACTOR = 20.0  # Macro Set: 20 drops = 1 mL
PRESCRIBED_FLOW = 120.0  # 40 DPM = 120 mL/h


def run_simulator(mode="normal", initial_vol=500.0, interval=5.0, api_url=API_URL, device_id=DEVICE_ID):
    print("=" * 75)
    print("  AMAL JYOTHI COLLEGE OF ENGINEERING - MCA 24MCAT213")
    print("  ESP32 Edge Microcontroller Telematics Simulator")
    print(f"  Target Device: {device_id} | Mode: {mode.upper()} | Init Vol: {initial_vol} mL")
    print(f"  Gateway API Endpoint: {api_url} | Interval: {interval}s")
    print("=" * 75)

    current_vol = float(initial_vol)
    counter = 0

    while True:
        counter += 1

        # Simulate physics & status based on selected test mode (Report Table 7.1)
        if mode == "normal":
            # Target prescribed rate ~40 DPM (120 mL/h)
            dpm = round(random.uniform(39.0, 41.5), 1)
            status = "NORMAL"
        elif mode == "no_flow" or mode == "blocked":
            # Tube clamped or line empty (>30s no drop)
            dpm = 0.0
            status = "NO_FLOW"
        elif mode == "free_flow" or mode == "rapid":
            # Runaway free flow (>150 DPM)
            dpm = round(random.uniform(155.0, 175.0), 1)
            status = "FREE_FLOW"
        elif mode == "low_volume" or mode == "depleted":
            dpm = round(random.uniform(38.0, 42.0), 1)
            current_vol = min(current_vol, 45.0)  # <= 10% of 500 mL
            status = "LOW_VOLUME"
        elif mode == "rate_err":
            # Flow outside +/-20% of 120 mL/h (e.g. 60 DPM = 180 mL/h)
            dpm = round(random.uniform(58.0, 65.0), 1)
            status = "RATE_ERR"
        elif mode == "dynamic":
            # Alternates through realistic clinical phases
            if counter < 6:
                dpm = round(random.uniform(39.0, 41.0), 1)
                status = "NORMAL"
            elif counter < 11:
                dpm = 0.0  # Occlusion / no flow
                status = "NO_FLOW"
            elif counter < 16:
                dpm = round(random.uniform(160.0, 170.0), 1)  # Free flow
                status = "FREE_FLOW"
            elif counter < 22:
                dpm = round(random.uniform(60.0, 65.0), 1)  # Rate deviation
                status = "RATE_ERR"
            else:
                dpm = round(random.uniform(39.5, 40.5), 1)
                status = "NORMAL"
        else:
            dpm = round(random.uniform(38.0, 42.0), 1)
            status = "NORMAL"

        # Volume decrement calculation:
        # Drops in interval = (DPM / 60) * interval
        # mL consumed = drops / DRIP_FACTOR
        drops_in_interval = (dpm / 60.0) * interval
        ml_consumed = drops_in_interval / DRIP_FACTOR
        current_vol = max(0.0, current_vol - ml_consumed)
        vol_infused = max(0.0, float(initial_vol) - current_vol)

        # Flow in mL/h = DPM * 60 / 20 = DPM * 3
        flow_ml_hr = round((dpm * 60.0) / DRIP_FACTOR, 1)

        if current_vol <= 30.0 and status != "NO_FLOW":
            status = "LOW_VOLUME"

        payload = {
            "device_id": device_id,
            "dpm": dpm,
            "flow_ml_hr": flow_ml_hr,
            "vol_infused_ml": round(vol_infused, 1),
            "vol_remaining_ml": round(current_vol, 1),
            "container_vol_ml": float(initial_vol),
            "status": status
        }

        try:
            start_time = time.time()
            resp = requests.post(api_url, json=payload, timeout=8.0)
            latency = (time.time() - start_time) * 1000

            if resp.status_code == 201:
                res_data = resp.json().get('data', {})
                print(f"[{time.strftime('%X')}] [ID: {res_data.get('id', '-')}] {device_id} | "
                      f"Flow: {dpm:5.1f} DPM ({flow_ml_hr:5.1f} mL/h) | Vol: {current_vol:5.1f} mL | "
                      f"Status: {status:<10} | RTT: {latency:4.0f}ms")
            else:
                print(f"[!] Gateway returned HTTP {resp.status_code}: {resp.text}")

        except requests.exceptions.RequestException as err:
            print(f"[x] Offline network error connecting to {api_url}: {err}")

        time.sleep(interval)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Simulate ESP32 IV Telematics Edge Node")
    parser.add_argument('--mode', choices=['normal', 'no_flow', 'blocked', 'free_flow', 'rapid', 'low_volume', 'depleted', 'rate_err', 'dynamic'],
                        default='normal', help='Simulation scenario to execute (Report Table 7.1)')
    parser.add_argument('--vol', type=float, default=500.0, help='Initial bottle volume in mL')
    parser.add_argument('--interval', type=float, default=5.0, help='Telemetry dispatch interval in seconds (default: 5s)')
    parser.add_argument('--url', type=str, default=API_URL, help='API endpoint URL (default: http://localhost:5000/api/telemetry)')
    parser.add_argument('--device', type=str, default=DEVICE_ID, help='Device/Bed ID (default: WARD-A-BED-07)')

    args = parser.parse_args()
    try:
        run_simulator(mode=args.mode, initial_vol=args.vol, interval=args.interval, api_url=args.url, device_id=args.device)
    except KeyboardInterrupt:
        print("\n[i] Simulation stopped by user.")
