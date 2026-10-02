#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A stand-in for YouGame's keyframe endpoint (ORCA.md, "Keyframe store"), for the OrcaKeyframe
# tests: python3 Tools/orca/fake_keyframes.py <port>, then
# ORCA_TEST_KEYFRAME_SERVER=http://127.0.0.1:<port> ./Binaries/Tests/tests --gtest_filter='OrcaKeyframe*'
#
#   PUT    /api/orca/keyframes/<room>/<id>         Authorization: Ticket good; 413 above LIMIT bytes
#   POST   /api/orca/keyframes/<room>/<id>/upload  -> {"url": "http://127.0.0.1:<port>/direct/<room>/<id>"}
#   PUT    /direct/<room>/<id>                     (the presigned upload: no Authorization)
#   GET    /api/orca/keyframes/<room>/<id>
#   DELETE /api/orca/keyframes/<room>/<id>
# A ticket other than "good" gets 401. Room "flaky" answers 503 to every other request.
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LIMIT = 1 << 20
store = {}
flaky = {"n": 0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def parts(self):
        p = self.path.strip("/").split("/")
        return p

    def check(self):
        p = self.parts()
        if len(p) >= 5 and p[3] == "flaky":
            flaky["n"] += 1
            if flaky["n"] % 2 == 1:
                self.reply(503, b'{"error":"busy"}')
                return False
        if self.path.startswith("/direct/"):
            return True
        if self.headers.get("Authorization") != "Ticket good":
            self.reply(401, b'{"error":"Bad or expired ticket"}')
            return False
        return True

    def body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n)

    def do_PUT(self):
        if not self.check():
            return
        p = self.parts()
        data = self.body()
        if p[0] == "direct":
            store[(p[1], p[2])] = data
            return self.reply(200, b"{}")
        if len(data) > LIMIT:
            return self.reply(413, b'{"error":"too large"}')
        store[(p[3], p[4])] = data
        self.reply(200, json.dumps({"id": p[4], "size": len(data)}).encode())

    def do_POST(self):
        if not self.check():
            return
        p = self.parts()
        self.body()
        url = "http://127.0.0.1:%d/direct/%s/%s" % (self.server.server_port, p[3], p[4])
        self.reply(200, json.dumps({"url": url}).encode())

    def do_GET(self):
        if not self.check():
            return
        p = self.parts()
        data = store.get((p[3], p[4]))
        if data is None:
            return self.reply(404, b'{"error":"no such keyframe"}')
        self.reply(200, data, "application/octet-stream")

    def do_DELETE(self):
        if not self.check():
            return
        p = self.parts()
        existed = store.pop((p[3], p[4]), None) is not None
        self.reply(204 if existed else 404)


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
