"""
========================================================================================
Project: Smart IoT IV Fluid Infusion Monitoring and Drip-Rate Telematics System
Institution: Amal Jyothi College of Engineering, Kanjirappally
Course: Master of Computer Applications (MCA) | 24MCAT213 - Internet of Things
Component: RESTful Telematics Backend API Gateway
Architecture: ESP32 DevKit V1 (TCRT5000 IR DO->GPIO19, Buzzer->GPIO18) -> Flask -> PostgreSQL
========================================================================================
"""

import os
import time
import logging
import sqlite3
from datetime import datetime, timezone
from urllib.parse import urlparse
from flask import Flask, request, jsonify, render_template
from flask_cors import CORS

# Configure structured clinical logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] [IV-GATEWAY] %(message)s'
)
logger = logging.getLogger(__name__)

app = Flask(__name__, template_folder='templates', static_folder='static')
app.config['TEMPLATES_AUTO_RELOAD'] = True
app.config['SEND_FILE_MAX_AGE_DEFAULT'] = 0
CORS(app)

# Database Configuration
DEFAULT_DB_URL = "postgresql://postgres:postgres@localhost:5432/iv_telematics"
DATABASE_URL = os.environ.get("DATABASE_URL", DEFAULT_DB_URL)
SQLITE_DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "iv_telematics.db")

# Global State
db_engine = "POSTGRES"  # Either 'POSTGRES' or 'SQLITE'
pg_pool = None

# Bedside Remote Alarm Silence Registry: { device_id: expiry_epoch }
bedside_silence_registry = {}

# Bedside IV Container Volume Registry: { device_id: bottle_volume_ml }
device_container_registry = {'WARD-A-BED-07': 200.0}

def is_bedside_silenced(device_id: str):
    """Checks if a device has an active remote silence acknowledgment."""
    dev = str(device_id or "").strip().upper()
    expiry = bedside_silence_registry.get(dev, 0)
    now = time.time()
    if expiry > now:
        return True, int(expiry - now)
    return False, 0


def parse_db_url(url: str):
    """Parse standard postgresql:// URI into connection parameters."""
    result = urlparse(url)
    return {
        'database': result.path[1:] if result.path else 'iv_telematics',
        'user': result.username or 'postgres',
        'password': result.password or 'postgres',
        'host': result.hostname or 'localhost',
        'port': result.port or 5432
    }


def init_db(max_retries=5, retry_delay=2):
    """
    Initialize database connection and ensure schema exists.
    Creates primary table 'infusion_readings' (matching MCA Project Report Table 9.2).
    """
    global db_engine, pg_pool

    if "db" in DATABASE_URL or os.environ.get("DOCKER_CONTAINER"):
        max_retries = 15

    db_params = parse_db_url(DATABASE_URL)
    logger.info("Connecting to database: %s:%s/%s",
                db_params['host'], db_params['port'], db_params['database'])

    try:
        import psycopg2
        from psycopg2 import pool, extras

        for attempt in range(1, max_retries + 1):
            try:
                pg_pool = psycopg2.pool.ThreadedConnectionPool(
                    minconn=2,
                    maxconn=20,
                    host=db_params['host'],
                    port=db_params['port'],
                    dbname=db_params['database'],
                    user=db_params['user'],
                    password=db_params['password'],
                    connect_timeout=3
                )

                # Initialize schema
                conn = pg_pool.getconn()
                with conn.cursor() as cur:
                    cur.execute("""
                        CREATE TABLE IF NOT EXISTS infusion_readings (
                            id BIGSERIAL PRIMARY KEY,
                            device_id VARCHAR(32) NOT NULL,
                            ts TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
                            dpm REAL NOT NULL,
                            flow_ml_hr REAL NOT NULL,
                            vol_infused_ml REAL NOT NULL,
                            vol_remaining_ml REAL NOT NULL,
                            status VARCHAR(20) NOT NULL
                        );
                        CREATE INDEX IF NOT EXISTS idx_infusion_dev_ts 
                            ON infusion_readings(device_id, ts DESC);
                        
                        -- Legacy table alias for backward compatibility
                        CREATE TABLE IF NOT EXISTS telematics_logs (
                            id SERIAL PRIMARY KEY,
                            bed_id VARCHAR(20) NOT NULL,
                            dpm FLOAT NOT NULL,
                            remaining_ml FLOAT NOT NULL,
                            status VARCHAR(20) NOT NULL,
                            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                        );
                    """)
                    conn.commit()
                pg_pool.putconn(conn)

                db_engine = "POSTGRES"
                logger.info("PostgreSQL storage active. Table 'infusion_readings' ready.")
                return True

            except Exception as err:
                logger.warning("PostgreSQL attempt %d/%d failed: %s. Retrying in %ds...",
                               attempt, max_retries, err, retry_delay)
                time.sleep(retry_delay)

    except ImportError:
        logger.warning("psycopg2 not installed; falling back to SQLite.")

    # Graceful SQLite fallback for local developer testing
    logger.info("Initializing fallback SQLite storage: %s", SQLITE_DB_PATH)
    db_engine = "SQLITE"
    sqlite_conn = None
    try:
        sqlite_conn = sqlite3.connect(SQLITE_DB_PATH)
        cur = sqlite_conn.cursor()
        cur.execute("""
            CREATE TABLE IF NOT EXISTS infusion_readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                dpm REAL NOT NULL,
                flow_ml_hr REAL NOT NULL,
                vol_infused_ml REAL NOT NULL,
                vol_remaining_ml REAL NOT NULL,
                status TEXT NOT NULL
            );
        """)
        cur.execute("CREATE INDEX IF NOT EXISTS idx_infusion_dev_ts ON infusion_readings(device_id, ts DESC);")
        cur.execute("""
            CREATE TABLE IF NOT EXISTS telematics_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                bed_id TEXT NOT NULL,
                dpm REAL NOT NULL,
                remaining_ml REAL NOT NULL,
                status TEXT NOT NULL,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            );
        """)
        sqlite_conn.commit()
        cur.close()
        sqlite_conn.close()
        sqlite_conn = None
        logger.info("SQLite storage initialized successfully.")
        return True
    except Exception as e:
        if sqlite_conn:
            sqlite_conn.close()
        logger.critical("Failed to initialize SQLite storage: %s", e)
        return False


def get_db_connection():
    """Returns an active database connection and cursor."""
    if db_engine == "POSTGRES" and pg_pool:
        import psycopg2.extras
        conn = pg_pool.getconn()
        return conn, conn.cursor(cursor_factory=psycopg2.extras.DictCursor)
    else:
        conn = sqlite3.connect(SQLITE_DB_PATH)
        conn.row_factory = sqlite3.Row
        return conn, conn.cursor()


def release_db_connection(conn):
    """Releases or closes connection."""
    if not conn:
        return
    if db_engine == "POSTGRES" and pg_pool:
        pg_pool.putconn(conn)
    else:
        conn.close()


def normalize_status(status_str, dpm, vol_remaining):
    """Normalizes status to match Report Table 7.1 and Table 9.2."""
    s = str(status_str or "").strip().upper()

    # Physical telemetry thresholds take precedence over incorrect/stale status strings
    if vol_remaining <= 50.0:  # <= 10% of 500mL
        return "LOW_VOLUME"
    if dpm > 150.0:
        return "FREE_FLOW"
    if dpm <= 1.0:  # No drops detected or line blocked
        return "NO_FLOW"

    if s in {"NORMAL", "LOW_VOLUME", "NO_FLOW", "FREE_FLOW", "RATE_ERR"}:
        return s
    
    # Map legacy status strings to report schema
    if s == "BLOCKED": return "NO_FLOW"
    if s == "DEPLETED": return "LOW_VOLUME"
    if s == "RAPID": return "FREE_FLOW"

    return "NORMAL"


# ======================================================================================
# API ENDPOINTS (Matching Report Table 9.1)
# ======================================================================================

@app.route('/')
def index():
    """Render the clinical web telemetry dashboard."""
    return render_template('index.html')


@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check for the container and database connection (Report Table 9.1)."""
    conn = None
    db_status = f"connected ({db_engine})"
    try:
        conn, cur = get_db_connection()
        cur.execute("SELECT 1;")
    except Exception as e:
        db_status = f"unhealthy: {str(e)}"
    finally:
        if conn:
            release_db_connection(conn)

    return jsonify({
        "status": "UP" if "unhealthy" not in db_status else "DEGRADED",
        "service": "Smart IV Infusion Telematics API Gateway",
        "database_engine": db_engine,
        "database_status": db_status,
        "timestamp": datetime.now(timezone.utc).isoformat()
    }), 200 if "unhealthy" not in db_status else 503


@app.route('/api/telemetry', methods=['POST'])
@app.route('/api/telematics', methods=['POST'])
def ingest_telemetry():
    """
    Ingest one JSON reading from an ESP32 node and insert into PostgreSQL (Report Table 9.1).
    Payload:
    {
        "device_id": "WARD-A-BED-07", // or "bed_id"
        "dpm": 40.0,
        "flow_ml_hr": 120.0,           // optional (auto-computed as DPM * 3 if missing)
        "vol_infused_ml": 128.0,       // optional
        "vol_remaining_ml": 372.0,     // or "remaining_ml"
        "status": "NORMAL"
    }
    """
    if not request.is_json:
        return jsonify({"status": "error", "message": "Content-Type must be application/json"}), 400

    payload = request.get_json(silent=True) or {}

    # Extract device / bed ID
    device_id = str(payload.get('device_id') or payload.get('bed_id') or 'WARD-A-BED-07').strip().upper()
    
    try:
        dpm = max(0.0, float(payload.get('dpm', 0.0)))
        
        # Check if packet contains container/bottle volume
        if payload.get('container_vol_ml') or payload.get('bottle_vol_ml'):
            try:
                c_vol = float(payload.get('container_vol_ml') or payload.get('bottle_vol_ml'))
                if 50.0 <= c_vol <= 2000.0:
                    device_container_registry[device_id] = c_vol
            except (ValueError, TypeError):
                pass

        total_container_vol = device_container_registry.get(device_id, 200.0)

        # Remaining volume (accepts vol_remaining_ml or remaining_ml)
        vol_remaining = float(payload.get('vol_remaining_ml') or payload.get('remaining_ml') or total_container_vol)
        vol_remaining = max(0.0, vol_remaining)
        
        # Flow rate in mL/hr: DPM * 60 / 20 = DPM * 3 (Macro 20 gtt/mL)
        flow_ml_hr = float(payload.get('flow_ml_hr') or (dpm * 3.0))
        
        # Infused volume
        vol_infused = float(payload.get('vol_infused_ml') or max(0.0, total_container_vol - vol_remaining))
    except (ValueError, TypeError):
        return jsonify({"status": "error", "message": "Numeric fields must be valid numbers."}), 422

    status = normalize_status(payload.get('status'), dpm, vol_remaining)

    conn = None
    try:
        conn, cur = get_db_connection()
        created_at_str = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

        if db_engine == "POSTGRES":
            cur.execute("""
                INSERT INTO infusion_readings 
                (device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts)
                VALUES (%s, %s, %s, %s, %s, %s, NOW())
                RETURNING id, ts;
            """, (device_id, dpm, flow_ml_hr, vol_infused, vol_remaining, status))
            row = cur.fetchone()
            new_id = row[0]
            ts_val = row[1].isoformat()

            # Mirror to legacy table
            cur.execute("""
                INSERT INTO telematics_logs (bed_id, dpm, remaining_ml, status, created_at)
                VALUES (%s, %s, %s, %s, NOW());
            """, (device_id, dpm, vol_remaining, status))
        else:
            cur.execute("""
                INSERT INTO infusion_readings 
                (device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts)
                VALUES (?, ?, ?, ?, ?, ?, ?);
            """, (device_id, dpm, flow_ml_hr, vol_infused, vol_remaining, status, created_at_str))
            new_id = cur.lastrowid
            ts_val = created_at_str

            cur.execute("""
                INSERT INTO telematics_logs (bed_id, dpm, remaining_ml, status, created_at)
                VALUES (?, ?, ?, ?, ?);
            """, (device_id, dpm, vol_remaining, status, created_at_str))

        conn.commit()

        logger.info("[INGEST] %s | Flow: %.1f DPM (%.1f mL/h) | Rem: %.1f/%.1f mL | %s (ID: %d)",
                    device_id, dpm, flow_ml_hr, vol_remaining, total_container_vol, status, new_id)

        silenced, rem_sec = is_bedside_silenced(device_id)

        return jsonify({
            "status": "success",
            "message": "Telemetry reading logged successfully.",
            "data": {
                "id": new_id,
                "device_id": device_id,
                "bed_id": device_id,
                "dpm": dpm,
                "flow_ml_hr": flow_ml_hr,
                "vol_infused_ml": vol_infused,
                "vol_remaining_ml": vol_remaining,
                "remaining_ml": vol_remaining,
                "container_vol_ml": total_container_vol,
                "bottle_vol_ml": total_container_vol,
                "status": status,
                "ts": ts_val
            },
            "bedside_silenced": silenced,
            "silence_sec": rem_sec
        }), 201

    except Exception as err:
        if conn:
            conn.rollback()
        logger.error("DB persistence error: %s", err)
        return jsonify({"status": "error", "message": "Database write failure."}), 500
    finally:
        if conn:
            release_db_connection(conn)


@app.route('/api/latest', defaults={'device_id': None}, methods=['GET'])
@app.route('/api/latest/<device_id>', methods=['GET'])
def get_latest_telemetry(device_id):
    """Returns the most recent reading and alert state of a bed (Report Table 9.1)."""
    if not device_id:
        device_id = request.args.get('device_id') or request.args.get('bed_id') or 'WARD-A-BED-07'
    device_id = str(device_id).strip().upper()

    conn = None
    try:
        conn, cur = get_db_connection()
        query = """
            SELECT id, device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts
            FROM infusion_readings
            WHERE device_id = %s
            ORDER BY ts DESC, id DESC
            LIMIT 1;
        """ if db_engine == "POSTGRES" else """
            SELECT id, device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts
            FROM infusion_readings
            WHERE device_id = ?
            ORDER BY ts DESC, id DESC
            LIMIT 1;
        """
        cur.execute(query, (device_id,))
        row = cur.fetchone()

        total_container = device_container_registry.get(device_id, 200.0)

        if not row:
            return jsonify({
                "status": "nodata",
                "message": f"No telemetry records logged yet for {device_id}.",
                "data": {
                    "device_id": device_id,
                    "bed_id": device_id,
                    "dpm": 0.0,
                    "flow_ml_hr": 0.0,
                    "vol_infused_ml": 0.0,
                    "vol_remaining_ml": total_container,
                    "remaining_ml": total_container,
                    "container_vol_ml": total_container,
                    "bottle_vol_ml": total_container,
                    "status": "NO_FLOW",
                    "ts": datetime.now(timezone.utc).isoformat()
                }
            }), 200

        ts_val = row['ts'].isoformat() if hasattr(row['ts'], 'isoformat') else str(row['ts'])
        silenced, rem_sec = is_bedside_silenced(device_id)

        # Compute how many seconds ago this record was written
        # Used by dashboard to determine ONLINE vs OFFLINE (threshold: 45s)
        try:
            if hasattr(row['ts'], 'tzinfo') and row['ts'].tzinfo is not None:
                last_seen_age = max(0, int((datetime.now(timezone.utc) - row['ts']).total_seconds()))
            else:
                # Naive datetime (SQLite) - treat as UTC
                from datetime import timezone as tz
                naive_ts = row['ts'] if hasattr(row['ts'], 'year') else datetime.fromisoformat(str(row['ts']))
                last_seen_age = max(0, int((datetime.now(timezone.utc).replace(tzinfo=None) - naive_ts).total_seconds()))
        except Exception:
            last_seen_age = 0

        return jsonify({
            "status": "success",
            "data": {
                "id": row['id'],
                "device_id": row['device_id'],
                "bed_id": row['device_id'],
                "dpm": round(float(row['dpm']), 1),
                "flow_ml_hr": round(float(row['flow_ml_hr']), 1),
                "vol_infused_ml": round(float(row['vol_infused_ml']), 1),
                "vol_remaining_ml": round(float(row['vol_remaining_ml']), 1),
                "remaining_ml": round(float(row['vol_remaining_ml']), 1),
                "container_vol_ml": total_container,
                "bottle_vol_ml": total_container,
                "status": row['status'],
                "ts": ts_val,
                "created_at": ts_val,
                "last_seen_seconds_ago": last_seen_age,
                "bedside_silenced": silenced,
                "silence_sec": rem_sec
            },
            "bedside_silenced": silenced,
            "silence_sec": rem_sec
        }), 200

    except Exception as err:
        logger.error("Error in /api/latest: %s", err)
        return jsonify({"status": "error", "message": "Database query failure."}), 500
    finally:
        if conn:
            release_db_connection(conn)


@app.route('/api/container', methods=['POST', 'GET'])
def manage_container_volume():
    """Configure or read the IV container volume (e.g. 150 mL, 200 mL, 500 mL)."""
    if request.method == 'POST':
        payload = request.get_json(silent=True) or {}
        device_id = str(payload.get('device_id') or 'WARD-A-BED-07').strip().upper()
        try:
            vol = float(payload.get('container_vol_ml') or payload.get('bottle_vol_ml') or 200.0)
            vol = max(50.0, min(vol, 2000.0))
            device_container_registry[device_id] = vol
            logger.info("[CONTAINER] Set %s bottle volume to %.1f mL", device_id, vol)
            return jsonify({
                "status": "success",
                "device_id": device_id,
                "container_vol_ml": vol,
                "bottle_vol_ml": vol,
                "message": f"Container volume updated to {vol:.0f} mL for {device_id}."
            }), 200
        except (ValueError, TypeError):
            return jsonify({"status": "error", "message": "Invalid volume numeric value."}), 422
    else:
        device_id = str(request.args.get('device_id') or 'WARD-A-BED-07').strip().upper()
        vol = device_container_registry.get(device_id, 200.0)
        return jsonify({
            "status": "success",
            "device_id": device_id,
            "container_vol_ml": vol,
            "bottle_vol_ml": vol
        }), 200


@app.route('/api/alarm/silence', methods=['POST'])
def remote_silence_alarm():
    """Remotely silence the bedside hardware buzzer for a duration (default 120s)."""
    payload = request.get_json(silent=True) or {}
    device_id = str(payload.get('device_id') or 'WARD-A-BED-07').strip().upper()
    duration_sec = int(payload.get('duration_sec', 120))
    action = payload.get('action', 'silence')

    if action == 'unsilence' or duration_sec <= 0:
        bedside_silence_registry.pop(device_id, None)
        logger.info("[ALARM] Bedside buzzer silence CLEARED for %s", device_id)
        return jsonify({
            "status": "success",
            "device_id": device_id,
            "bedside_silenced": False,
            "silence_sec": 0,
            "message": f"Bedside buzzer silence canceled for {device_id}."
        }), 200

    duration_sec = min(max(duration_sec, 10), 600)  # Between 10s and 10min
    expiry = time.time() + duration_sec
    bedside_silence_registry[device_id] = expiry
    logger.info("[ALARM] Bedside buzzer SILENCED for %s for %ds", device_id, duration_sec)

    return jsonify({
        "status": "success",
        "device_id": device_id,
        "bedside_silenced": True,
        "silence_sec": duration_sec,
        "message": f"Bedside buzzer silenced for {duration_sec}s for {device_id}."
    }), 200


@app.route('/api/history', defaults={'device_id': None}, methods=['GET'])
@app.route('/api/history/<device_id>', methods=['GET'])
@app.route('/api/telematics/history', methods=['GET'])
def get_telemetry_history(device_id):
    """Returns time-series readings for the live graph (Report Table 9.1)."""
    if not device_id:
        device_id = request.args.get('device_id') or request.args.get('bed_id') or 'WARD-A-BED-07'
    device_id = str(device_id).strip().upper()

    try:
        limit = min(max(int(request.args.get('limit', 30)), 5), 100)
    except ValueError:
        limit = 30

    conn = None
    try:
        conn, cur = get_db_connection()
        query = """
            SELECT id, device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts
            FROM (
                SELECT id, device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts
                FROM infusion_readings
                WHERE device_id = %s
                ORDER BY ts DESC, id DESC
                LIMIT %s
            ) sub
            ORDER BY ts ASC, id ASC;
        """ if db_engine == "POSTGRES" else """
            SELECT id, device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts
            FROM (
                SELECT id, device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts
                FROM infusion_readings
                WHERE device_id = ?
                ORDER BY ts DESC, id DESC
                LIMIT ?
            ) sub
            ORDER BY ts ASC, id ASC;
        """
        cur.execute(query, (device_id, limit))
        rows = cur.fetchall()

        history = []
        for row in rows:
            ts_val = row['ts'].isoformat() if hasattr(row['ts'], 'isoformat') else str(row['ts'])
            history.append({
                "id": row['id'],
                "device_id": row['device_id'],
                "bed_id": row['device_id'],
                "dpm": round(float(row['dpm']), 1),
                "flow_ml_hr": round(float(row['flow_ml_hr']), 1),
                "vol_infused_ml": round(float(row['vol_infused_ml']), 1),
                "vol_remaining_ml": round(float(row['vol_remaining_ml']), 1),
                "remaining_ml": round(float(row['vol_remaining_ml']), 1),
                "status": row['status'],
                "ts": ts_val,
                "created_at": ts_val
            })

        return jsonify({
            "status": "success",
            "device_id": device_id,
            "count": len(history),
            "data": history
        }), 200

    except Exception as err:
        logger.error("Error in /api/history: %s", err)
        return jsonify({"status": "error", "message": "Database query failure."}), 500
    finally:
        if conn:
            release_db_connection(conn)


@app.route('/api/devices', methods=['GET'])
def get_registered_devices():
    """Lists all registered beds / nodes and their last-seen time (Report Table 9.1)."""
    conn = None
    try:
        conn, cur = get_db_connection()
        query = """
            SELECT device_id, MAX(ts) as last_seen, COUNT(*) as packet_count
            FROM infusion_readings
            GROUP BY device_id
            ORDER BY last_seen DESC;
        """
        cur.execute(query)
        rows = cur.fetchall()

        devices = []
        for r in rows:
            last_seen_str = r['last_seen'].isoformat() if hasattr(r['last_seen'], 'isoformat') else str(r['last_seen'])
            devices.append({
                "device_id": r['device_id'],
                "last_seen": last_seen_str,
                "total_packets": r['packet_count']
            })

        return jsonify({
            "status": "success",
            "count": len(devices),
            "devices": devices
        }), 200
    except Exception as err:
        logger.error("Error in /api/devices: %s", err)
        return jsonify({"status": "error", "message": "Query failed."}), 500
    finally:
        if conn:
            release_db_connection(conn)


def seed_baseline_if_empty():
    """Seeds baseline records so dashboard displays instantly on fresh launch."""
    conn = None
    try:
        conn, cur = get_db_connection()
        dev_id = 'WARD-A-BED-07'
        query = "SELECT COUNT(*) FROM infusion_readings WHERE device_id = %s;" if db_engine == "POSTGRES" else "SELECT COUNT(*) FROM infusion_readings WHERE device_id = ?;"
        cur.execute(query, (dev_id,))
        count = cur.fetchone()[0]

        if count == 0:
            logger.info("Seeding initial baseline packets for %s...", dev_id)
            baseline = [
                (0.0, 0.0, 0.0, 200.0, 'NO_FLOW'),
            ]
            for dpm, flow, infused, remaining, stat in baseline:
                if db_engine == "POSTGRES":
                    cur.execute("""
                        INSERT INTO infusion_readings (device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status, ts)
                        VALUES (%s, %s, %s, %s, %s, %s, NOW());
                    """, (dev_id, dpm, flow, infused, remaining, stat))
                    cur.execute("""
                        INSERT INTO telematics_logs (bed_id, dpm, remaining_ml, status, created_at)
                        VALUES (%s, %s, %s, %s, NOW());
                    """, (dev_id, dpm, remaining, stat))
                else:
                    cur.execute("""
                        INSERT INTO infusion_readings (device_id, dpm, flow_ml_hr, vol_infused_ml, vol_remaining_ml, status)
                        VALUES (?, ?, ?, ?, ?, ?);
                    """, (dev_id, dpm, flow, infused, remaining, stat))
                    cur.execute("""
                        INSERT INTO telematics_logs (bed_id, dpm, remaining_ml, status)
                        VALUES (?, ?, ?, ?);
                    """, (dev_id, dpm, remaining, stat))
            conn.commit()
    except Exception as e:
        logger.warning("Seed note: %s", e)
    finally:
        if conn:
            release_db_connection(conn)


with app.app_context():
    if init_db(max_retries=1, retry_delay=1):
        seed_baseline_if_empty()


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=False)
