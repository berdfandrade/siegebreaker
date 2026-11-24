import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
from local_server import Handler, ThreadingHTTPServer

exe = sys.argv[1]
seen = []
class Observed(Handler):
    def do_GET(self):
        seen.append(self.client_address)
        super().do_GET()

server = ThreadingHTTPServer(("127.0.0.1", 0), Observed)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f"http://127.0.0.1:{server.server_port}"

def run(path, *args):
    result = subprocess.run([exe, "barrage", "--url", url + path, "--duration", "1s",
                             "--rps", "20", *args], capture_output=True, timeout=7)
    assert result.returncode == 0, result.stderr
    m = json.loads(result.stdout)
    assert m["planned"] == m["admitted"] + m["missed_schedule"] + m["pool_full"]
    assert m["admitted"] == m["completed"] + m["transport_errors"]
    return m

m = run("/", "--workers", "4", "--connections", "8")
assert len(seen) == m["completed"] == m["http"]["200"] == m["emitted"] > 10
assert len(set(seen)) == m["new_connections"] < len(seen)
assert m["scheduled_us"]["n"] == m["service_us"]["n"] == len(seen)
assert m["body_bytes"] == len(seen) * 3
seen.clear()
m = run("/close")
assert len(set(seen)) == m["new_connections"] == m["completed"] > 5
m = run("/error")
assert m["http"]["503"] == m["http_errors"] > 0 and not m["transport_errors"]
m = run("/slow", "--timeout-ms", "40")
assert m["timeouts"] == m["transport_errors"] > 0 and m["service_us"] is None
m = run("/slow", "--workers", "1", "--connections", "1", "--rps", "100")
assert m["pool_full"] > 50 and 1 <= m["completed"] <= 5
seen.clear()
p = subprocess.Popen([exe,"barrage","--url",url+"/slow","--duration","60s"],
                     stdout=subprocess.PIPE)
for _ in range(200):
    if seen or p.poll() is not None:
        break
    time.sleep(.01)
p.send_signal(signal.SIGTERM)
out, _ = p.communicate(timeout=4)
assert p.returncode == 143 and json.loads(out)["status"] == "interrupted"
for target in ["http://192.0.2.1", "file:///etc/hosts", "http://user:pass@localhost/"]:
    result = subprocess.run([exe,"barrage","--url",target],capture_output=True)
    assert result.returncode == 2 and b"pass@" not in result.stderr
server.shutdown()
server.server_close()
