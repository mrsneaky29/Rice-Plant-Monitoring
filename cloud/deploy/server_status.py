"""Read-only deployment status without credential values or photo contents."""
import json
import sqlite3

connection = sqlite3.connect('file:/var/lib/rice-cloud/rice-cloud.sqlite3?mode=ro', uri=True)
connection.row_factory = sqlite3.Row
try:
    for row in connection.execute('SELECT station_id,last_seen,next_wake_at,sleep_enabled,awake,status_json FROM stations ORDER BY station_id'):
        status = json.loads(row['status_json'])
        report = {key: row[key] for key in ['station_id', 'last_seen', 'next_wake_at', 'sleep_enabled', 'awake']}
        report.update({key: status.get(key) for key in ['camera_ok', 'sd_mounted', 'photo_count', 'last_upload_error', 'maintenance_remaining_seconds']})
        print(json.dumps(report))
    print('Stored photos by station:', [tuple(row) for row in connection.execute('SELECT station_id,count(*) FROM photos GROUP BY station_id')])
    print('Analysis jobs:', [tuple(row) for row in connection.execute('SELECT state,count(*) FROM analysis_jobs GROUP BY state')])
finally:
    connection.close()
