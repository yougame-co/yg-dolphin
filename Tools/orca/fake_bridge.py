# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A stand-in for the YouGame desktop app's loopback bridge, for testing Orca's room client:
# POST /sdk (hello, mpTicket via an anonymous dev ticket, mpRoom recorded), GET /events (SSE: the
# page's Leave two seconds after the room reads ready). Prints its port. Usage:
#   FAKE_ROOM=orcabr123 FAKE_LOG=/tmp/bridge.log python3 Tools/orca/fake_bridge.py &
#   ORCA_LIVE_TEST=1 ORCA_TEST_BRIDGE_ROOM=orcabr123 YOUGAME_BRIDGE=http://127.0.0.1:<port> \
#     YOUGAME_TOKEN=t0ken YOUGAME_GAME=orca-test ./Binaries/Tests/tests \
#     --gtest_filter='OrcaLive.BridgeMirrorsTheRoomAndTakesLeave'
# Run that test alone: with a bridge configured, every room goes through it.
import json, os, sys, threading, time, urllib.request, random, string
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOM = os.environ["FAKE_ROOM"]; TOKEN = os.environ.get("FAKE_TOKEN", "t0ken")
SLUG = os.environ.get("FAKE_SLUG", "orca-test"); LOG = os.environ["FAKE_LOG"]
# FAKE_COOKIE (a signed-in session's cookie header) and FAKE_TICKET_SLUG (a live Orca listing): real
# tickets, as the app gets them. FAKE_NO_LEAVE=1: the page never sends Leave.
COOKIE = os.environ.get("FAKE_COOKIE", ""); TICKET_SLUG = os.environ.get("FAKE_TICKET_SLUG", "")
SITE = os.environ.get("FAKE_SITE", "https://yougame.co"); NO_LEAVE = os.environ.get("FAKE_NO_LEAVE") == "1"
state = {"leave_at": None, "rooms": 0}
lock = threading.Lock()

def log(obj):
    with open(LOG, "a") as f: f.write(json.dumps(obj) + "\n")

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def authed(self):
        return self.headers.get("Authorization") == "Bearer " + TOKEN
    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code); self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        if self.path != "/sdk" or not self.authed(): return self.reply(401, {"ok": False, "error": "no"})
        msg = json.loads(self.rfile.read(int(self.headers["content-length"])))
        t = msg.get("type")
        if t == "hello": return self.reply(200, {"ok": True, "result": {"room": ROOM}})
        if t == "mpTicket":
            pid = "orcabr-" + "".join(random.choice(string.ascii_lowercase) for _ in range(12))
            headers = {"content-type": "application/json", "user-agent": "Mozilla/5.0 (Macintosh) OrcaTest"}
            if COOKIE:
                # A signed-in player's ticket for a real listing (the keyframe store refuses dev tickets).
                body = {"slug": TICKET_SLUG, "player_id": pid, "players": msg["players"], "mode": msg["mode"],
                        "room": msg["room"], "lobby": msg["lobby"]}
                headers["cookie"] = COOKIE
                headers["origin"] = SITE
            else:
                body = {"dev": "orca-bridge-test", "player_id": pid, "name": "Bridge", "players": msg["players"],
                        "mode": msg["mode"], "room": msg["room"], "lobby": msg["lobby"]}
            req = urllib.request.Request(SITE + "/api/multiplayer/ticket", data=json.dumps(body).encode(),
                headers=headers)
            res = json.loads(urllib.request.urlopen(req, timeout=15).read())
            log({"ticket_for": msg["room"], "mode": msg["mode"]})
            return self.reply(200, {"ok": True, "result": res})
        if t == "mpRoom":
            log({"mpRoom": msg})
            with lock:
                state["rooms"] += 1
                if msg.get("ready") and state["leave_at"] is None and not NO_LEAVE:
                    state["leave_at"] = time.time() + 2
            return self.reply(200, {"ok": True, "result": {}})
        return self.reply(200, {"ok": False, "error": "unknown type " + str(t)})
    def do_GET(self):
        if self.path != "/events" or not self.authed(): return self.reply(401, {"ok": False})
        self.send_response(200); self.send_header("content-type", "text/event-stream"); self.end_headers()
        self.wfile.write(b": connected\n\n"); self.wfile.flush()
        sent = False
        while True:
            time.sleep(0.2)
            with lock: at = state["leave_at"]
            try:
                if at and not sent and time.time() >= at:
                    self.wfile.write(("data: " + json.dumps({"slug": "someone-else", "cmd": "leave"}) + "\n\n").encode())
                    self.wfile.write(("data: " + json.dumps({"yg": 1, "slug": SLUG, "cmd": "leave"}) + "\n\n").encode())
                    self.wfile.flush(); sent = True; log({"sent": "leave"})
                else:
                    self.wfile.write(b": ping\n\n"); self.wfile.flush()
            except Exception:
                return

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
