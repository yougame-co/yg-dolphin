"""Wait for the finish-0348 load ceiling without abandoning the remaining work."""
import os
import subprocess
import time


def wait_for_load():
    started = time.monotonic()
    while True:
        subprocess.run(['uptime'], check=True)
        if os.getloadavg()[0] < 18:
            return
        if time.monotonic() - started >= 45 * 60:
            raise SystemExit('Load stayed at least 18 for 45 minutes.')
        print('Waiting 60 seconds for 1-minute load below 18.', flush=True)
        time.sleep(60)
