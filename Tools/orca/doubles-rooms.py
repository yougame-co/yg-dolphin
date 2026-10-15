#!/usr/bin/env python3
"""Four-instance friends-room fixture; dev tickets, isolated users, no app credentials.

  dropin-rooms.py --doubles --self-test
  dropin-rooms.py --doubles --serve --listen 127.0.0.1 --port 8765
  dropin-rooms.py --doubles --run --binary <nogui> --disc <disc> --site http://127.0.0.1:8765

For the lead's two-machine run, serve on an isolated LAN and run --roles 0,1 on
Mac and --roles 2,3 on PC, with the same --site and --room. --input is optional;
without it steer each isolated headless instance with stdin pad commands.
"""
import argparse
import base64
import hashlib
import ipaddress
import json
import os
import queue
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def isolated_host(host):
    if host == "localhost":
        return True
    try:
        address = ipaddress.ip_address(host)
        return address.is_loopback or (address.is_private and not address.is_unspecified)
    except ValueError:
        return False


def frame(payload, masked=False):
    data = json.dumps(payload).encode()
    header = bytes([0x81, (0x80 if masked else 0) | (len(data) if len(data) < 126 else 126)])
    if len(data) >= 126:
        header += struct.pack("!H", len(data))
    if masked:
        mask = b"test"
        return header + mask + bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
    return header + data


def receive(stream):
    header = stream.read(2)
    if len(header) != 2:
        raise EOFError
    opcode, size = header[0] & 15, header[1] & 127
    if size == 126:
        size = struct.unpack("!H", stream.read(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", stream.read(8))[0]
    if size > 65536:
        raise ValueError("fixture message exceeds limit")
    mask = stream.read(4) if header[1] & 128 else b""
    data = stream.read(size)
    if mask:
        data = bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
    if opcode == 8:
        raise EOFError
    return json.loads(data) if opcode == 1 else {}


class Rooms:
    def __init__(self):
        self.lock = threading.RLock()
        self.members = {}
        self.sockets = {}
        self.joined = set()
        self.offers = []
        self.captures = {}
        self.hashes = {}
        self.frames = {}
        self.reports = {}
        self.expired = set()
        self.blocked = set()

    def blackout(self, room, player):
        with self.lock:
            self.blocked.add((room, player))
            connection = self.sockets.get((room, player))
            if connection:
                connection[0].shutdown(socket.SHUT_RDWR)
            expiry = threading.Timer(90, self.expire, (room, player))
            expiry.daemon = True
            expiry.start()

    def expire(self, room, player):
        with self.lock:
            self.expired.add((room, player))
            self.members.pop((room, player), None)
            connection = self.sockets.pop((room, player), None)
            if connection:
                connection[0].shutdown(socket.SHUT_RDWR)
            for (code, name), other in list(self.sockets.items()):
                if code == room:
                    self.send(other, self.roster(room, name, "leave", player))

    def diagnostics(self, room):
        active = [name for code, name in self.members if code == room]
        reports = {name: value for (code, name), value in self.reports.items() if code == room}
        hashes = {name: value for (code, name), value in self.hashes.items() if code == room}
        return {"active": active, "reports": reports,
                "frames": {name: frame_number for (code, name), frame_number in self.frames.items() if code == room}, "hashes": {name: dict(value) for name, value in hashes.items()}}

    def record(self, room, player, data):
        packets = [entry[1] for entry in data.get("p", [])] if data.get("k") == "bp" else [data]
        for packet in packets:
            if packet.get("k") == "p":
                key = (room, player)
                self.frames[key] = max(self.frames.get(key, 0), packet.get("c", 0))
            if packet.get("k") == "p" and "x" in packet and "h" in packet:
                self.hashes.setdefault((room, player), {})[str(packet["x"])] = packet["h"]

    def roster(self, room, local, kind="welcome", changed=None, away=False):
        members = sorted(((seat, name) for (code, name), seat in self.members.items() if code == room))
        return {"t": kind, "me": local, "host": members[0][1], "lifecycle": "game", "revision": 1,
                "room": {"code": room, "queue": "private", "size": 4, "doubles": True},
                "player": {"id": changed or local},
                "players": [{"id": name, "name": name, **({"away": True} if away and name == changed else {})}
                            for _, name in members],
                "participants": [{"id": name, "connectionId": name, "slot": seat, "name": name}
                                 for seat, name in members]}

    def send(self, connection, data):
        sock, guard = connection
        with guard:
            sock.sendall(frame(data))

    def broadcast(self, room, local, message):
        for (code, name), connection in list(self.sockets.items()):
            if code == room and name != local:
                try:
                    self.send(connection, message)
                except OSError:
                    pass


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, turn_binary=None):
        self.rooms = Rooms()
        self.turn = None
        self.ice = []
        if turn_binary:
            self.turn = subprocess.Popen([turn_binary, address[0]], stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL, text=True)
            port = int(self.turn.stdout.readline())
            self.ice = [{"urls": f"turn:{address[0]}:{port}", "username": "orca-fixture",
                         "credential": "orca-fixture-only"}]
        super().__init__(address, Handler)

    def server_close(self):
        super().server_close()
        if self.turn:
            self.turn.terminate()
            self.turn.wait(timeout=5)
            self.turn.stdout.close()

    def handle_error(self, request, address):
        pass  # Expected fixture disconnects never print packet contents.


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, body, status=200):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path.startswith(("/report/", "/expire/", "/blackout/")):
            count = int(self.headers.get("Content-Length", 0))
            if count > 4096:
                return self.reply({}, 413)
            body = json.loads(self.rfile.read(count))
            room = self.path.rsplit("/", 1)[-1]
            player = body.get("player")
            if player not in {f"Doubles{seat}" for seat in range(4)}:
                return self.reply({}, 400)
            rooms = self.server.rooms
            with rooms.lock:
                if self.path.startswith("/blackout/"):
                    rooms.blackout(room, player)
                elif self.path.startswith("/expire/"):
                    rooms.expire(room, player)
                else:
                    rooms.reports[(room, player)] = body
            return self.reply({})
        if self.path != "/api/multiplayer/ticket":
            return self.reply({}, 404)
        count = int(self.headers.get("Content-Length", 0))
        if count > 4096:
            return self.reply({}, 413)
        body = json.loads(self.rfile.read(count))
        room, player = body["room"], body["player_id"]
        if body.get("dev") != "orca-doubles-test" or player not in {"Doubles0", "Doubles1", "Doubles2", "Doubles3"}:
            return self.reply({}, 403)
        with self.server.rooms.lock:
            members = self.server.rooms.members
            key = (room, player)
            if key in self.server.rooms.blocked:
                return self.reply({"error": "room-recovering"}, 409)
            if key in self.server.rooms.expired:
                return self.reply({"error": "room-hold-expired"}, 409)
            if key not in members:
                used = {seat for (code, _), seat in members.items() if code == room}
                if len(used) == 4:
                    return self.reply({}, 409)
                members[key] = min(set(range(4)) - used)
        # This literal is a throwaway local fixture value, never a production ticket.
        host = self.headers["Host"]
        self.reply({"url": "http://" + host, "ticket": player, "direct": True, "ice": self.server.ice})

    def do_GET(self):
        if self.path.startswith("/diagnostics/"):
            with self.server.rooms.lock:
                return self.reply(self.server.rooms.diagnostics(self.path.rsplit("/", 1)[-1]))
        if self.path.startswith("/api/orca/keyframes/"):
            return self.keyframe("get")
        parsed = urllib.parse.urlsplit(self.path)
        room = parsed.path.rsplit("/", 1)[-1]
        player = urllib.parse.parse_qs(parsed.query).get("ticket", [""])[0]
        rooms = self.server.rooms
        with rooms.lock:
            if (room, player) not in rooms.members:
                return self.reply({}, 403)
        self.send_response(101)
        self.send_header("Connection", "Upgrade")
        self.send_header("Upgrade", "websocket")
        accept = hashlib.sha1((self.headers["Sec-WebSocket-Key"] +
                               "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        self.send_header("Sec-WebSocket-Accept", base64.b64encode(accept).decode())
        self.end_headers()
        connection = (self.connection, threading.Lock())
        with rooms.lock:
            returning = (room, player) in rooms.joined
            rooms.joined.add((room, player))
            rooms.sockets[(room, player)] = connection
            rooms.send(connection, rooms.roster(room, player))
            for (code, name), other in list(rooms.sockets.items()):
                if code == room and name != player:
                    rooms.send(other, rooms.roster(room, name, "back" if returning else "join", player))
        departed = False
        try:
            while True:
                message = receive(self.rfile)
                with rooms.lock:
                    if message.get("t") == "ping":
                        rooms.send(connection, {"t": "pong", "at": message["at"]})
                    elif message.get("t") == "msg":
                        rooms.record(room, player, message["d"])
                        outgoing = {"t": "msg", "from": player, "d": message["d"]}
                        if message["d"].get("k") == "kf":
                            rooms.offers.append((message.get("to"), message["d"].get("id"), message["d"].get("f")))
                        target = message.get("to")
                        if target:
                            other = rooms.sockets.get((room, target))
                            if other:
                                rooms.send(other, outgoing)
                        else:
                            rooms.broadcast(room, player, outgoing)
                    elif message.get("t") == "leave":
                        rooms.members.pop((room, player), None)
                        for (code, name), other in list(rooms.sockets.items()):
                            if code == room and name != player:
                                rooms.send(other, rooms.roster(room, name, "leave", player))
                        departed = True
                        break
        except (EOFError, OSError, ValueError):
            pass
        finally:
            with rooms.lock:
                if rooms.sockets.get((room, player)) is connection:
                    rooms.sockets.pop((room, player), None)
                    if not departed and (room, player) in rooms.members:
                        for (code, name), other in list(rooms.sockets.items()):
                            if code == room and name != player:
                                try:
                                    rooms.send(other, rooms.roster(room, name, "away", player, True))
                                except OSError:
                                    pass
            self.close_connection = True

    def keyframe(self, operation):
        parts = self.path.split("/")
        if len(parts) != 6 or self.headers.get("Authorization") not in {
                f"Ticket Doubles{seat}" for seat in range(4)}:
            return self.reply({}, 403)
        capture = (parts[4], parts[5])
        rooms = self.server.rooms
        with rooms.lock:
            if operation == "put":
                count = int(self.headers.get("Content-Length", 0))
                if count > 128 * 1024 * 1024:
                    return self.reply({}, 413)
                if capture in rooms.captures:
                    return self.reply({"code": "exists"}, 409)
                rooms.captures[capture] = self.rfile.read(count)
                return self.reply({"id": capture[1], "size": count})
            if operation == "delete":
                rooms.captures.pop(capture, None)
                return self.reply({})
            data = rooms.captures.get(capture)
            if data is None:
                return self.reply({"code": "gone"}, 404)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        self.keyframe("put")

    def do_DELETE(self):
        self.keyframe("delete")


def client(site, player, room):
    request = urllib.request.Request(site + "/api/multiplayer/ticket", json.dumps(
        {"dev": "orca-doubles-test", "room": room, "player_id": player}).encode(),
        {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=3) as response:
        json.load(response)
    address = urllib.parse.urlsplit(site)
    sock = socket.create_connection((address.hostname, address.port), timeout=3)
    sock.sendall((f"GET /room/{room}?ticket={player} HTTP/1.1\r\nHost: {address.netloc}\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                  "Sec-WebSocket-Key: dGVzdGRvdWJsZXNmaXh0dXJl\r\n\r\n").encode())
    stream = sock.makefile("rb")
    assert b"101" in stream.readline()
    while stream.readline().strip():
        pass
    return sock, stream


def self_test(turn_binary=None):
    server = Server(("127.0.0.1", 0), turn_binary)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    site = f"http://127.0.0.1:{server.server_port}"
    clients = []
    try:
        for seat in range(4):
            clients.append(client(site, f"Doubles{seat}", "dry444"))
            welcome = receive(clients[-1][1])
            assert welcome["room"]["doubles"] and welcome["room"]["queue"] == "private"
            assert welcome["participants"][-1]["slot"] == seat
        for seat, (sock, stream) in enumerate(clients):
            # Drain the expected subsequent roster joins, then check the complete mesh fan-out.
            for _ in range(3 - seat):
                assert receive(stream)["t"] == "join"
            sock.sendall(frame({"t": "msg", "d": {"k": "hello", "doubles": True}}, True))
        for seat, (_, stream) in enumerate(clients):
            senders = {receive(stream)["from"] for _ in range(3)}
            assert senders == {f"Doubles{other}" for other in range(4) if other != seat}
        for seat in range(1, 4):
            clients[0][0].sendall(frame({"t": "msg", "to": f"Doubles{seat}",
                                       "d": {"k": "kf", "id": "dry-capture", "f": 240}}, True))
            assert receive(clients[seat][1])["d"]["f"] == 240
        assert len({capture for _, capture, _ in server.rooms.offers}) == 1
        # An ungraceful disconnect retains the seat and code; reconnect replaces only that socket.
        clients[3][0].shutdown(socket.SHUT_RDWR)
        clients[3][1].close()
        clients[3][0].close()
        for _, stream in clients[:3]:
            assert receive(stream)["t"] == "away"
        clients[3] = client(site, "Doubles3", "dry444")
        resumed = receive(clients[3][1])
        assert resumed["room"]["code"] == "dry444" and resumed["participants"][-1]["slot"] == 3
        for _, stream in clients[:3]:
            assert receive(stream)["t"] == "back"
        clients[3][0].sendall(frame({"t": "leave"}, True))
        for _, stream in clients[:3]:
            verdict = receive(stream)
            assert verdict["t"] == "leave" and len(verdict["participants"]) == 3
        clients[2][0].shutdown(socket.SHUT_RDWR)
        clients[2][1].close()
        clients[2][0].close()
        for _, stream in clients[:2]:
            assert receive(stream)["t"] == "away"
        server.rooms.expire("dry444", "Doubles2")
        for _, stream in clients[:2]:
            verdict = receive(stream)
            assert verdict["t"] == "leave" and verdict["player"]["id"] == "Doubles2"
        for role in range(3):
            server.rooms.reports[("dry444", f"Doubles{role}")] = {"port": role + 1, "plug": 360 if role else None}
            server.rooms.record("dry444", f"Doubles{role}", {"k": "p", "x": 600, "h": "fixture-hash"})
        verify_diagnostics(server.rooms.diagnostics("dry444"), expected=3)
        bad = server.rooms.diagnostics("dry444")
        bad["reports"]["Doubles2"] = {"port": 3, "plug": 361}
        try:
            verify_diagnostics(bad, expected=3)
            raise AssertionError("different cross-machine plug frames accepted")
        except ValueError:
            pass
        bad = server.rooms.diagnostics("dry444")
        bad["hashes"]["Doubles1"] = {"600": "different"}
        try:
            verify_diagnostics(bad, expected=3)
            raise AssertionError("different final hashes accepted")
        except ValueError:
            pass
        print("PASS: four clients, six pairs, shared capture, recovery, leave, hold expiry, cross-machine plugs and hashes")
    finally:
        for sock, stream in clients:
            stream.close()
            sock.close()
        server.shutdown()
        server.server_close()


def verify_diagnostics(data, expected=4):
    reports = data["reports"]
    if len(reports) < expected:
        raise ValueError("waiting for reports from both machines")
    plugs = [report["plug"] for report in reports.values() if report.get("plug") is not None]
    if not plugs or len(set(plugs)) != 1:
        raise ValueError("cross-machine plug frames differ")
    ports = [report["port"] for report in reports.values() if report.get("port") is not None]
    if len(ports) != len(set(ports)):
        raise ValueError("duplicate ports across machines")
    active = data["active"]
    hashes = [data["hashes"].get(name, {}) for name in active]
    common = set.intersection(*(set(values) for values in hashes)) if hashes else set()
    if not common:
        raise ValueError("no common confirmed frame hash")
    for frame_number in common:
        if len({values[frame_number] for values in hashes}) != 1:
            raise ValueError(f"confirmed frame {frame_number} differs across machines")
    latest = max(map(int, common))
    if latest < min(data["frames"].get(name, 0) for name in active) - 120:
        raise ValueError("common confirmed hashes stopped before the end of the run")
    return latest


def post_fixture(site, path, body):
    request = urllib.request.Request(site + path, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=3) as response:
        return json.load(response)


def run(args):
    address = urllib.parse.urlsplit(args.site)
    if address.scheme != "http" or not isolated_host(address.hostname) or address.username or address.password:
        raise ValueError("--site must be a loopback or isolated LAN dev fixture")
    if args.hold_expiry and args.seconds < 120:
        raise ValueError("--hold-expiry needs at least 120 seconds for the 90-second hold")
    roles = [int(role) for role in args.roles.split(",")]
    if len(set(roles)) != len(roles) or not set(roles) <= set(range(4)):
        raise ValueError("roles must be distinct values from 0..3")
    players = []
    with tempfile.TemporaryDirectory(prefix="orca-doubles-") as work:
        try:
            for role in roles:
                user = Path(work) / f"user-{role}"
                user.mkdir()
                environment = {"PATH": os.environ.get("PATH", ""),
                               "ORCA_SESSION": "1", "ORCA_TEST_COMMANDS": "1", "ORCA_TEST_ROOM_DROP": "1",
                               "ORCA_TEST_DEV_GAME": "orca-doubles-test", "ORCA_SITE": args.site,
                               "ORCA_ROOM": args.room, "ORCA_JOIN": "0" if role == 0 else "1",
                               "ORCA_TEST_PLAYER": f"Doubles{role}", "ORCA_NAME": f"Doubles{role}",
                               "ORCA_DIRECT": "1", "ORCA_DIRECT_LOOPBACK": "1", "ORCA_DIRECT_STUN": "0"}
                if args.transport == "turn":
                    environment["ORCA_DIRECT_FORCE"] = "turn"
                if os.name == "nt":
                    environment["SystemRoot"] = os.environ.get("SystemRoot", r"C:\Windows")
                if args.input:
                    environment["YG_INPUT"] = str(Path(args.input).resolve())
                if args.profile:
                    environment["ORCA_PROFILE"] = args.profile
                if args.mod_dir:
                    environment["ORCA_MOD_DIR"] = str(Path(args.mod_dir).resolve())
                    environment["ORCA_SD_CACHE"] = str(Path(work) / "sd-cache")
                stderr = open(Path(work) / f"peer-{role}.log", "w")
                command = [args.binary, "-p", "headless", "-u", str(user), "-v", "Null",
                           "-C", "Dolphin.DSP.Backend=No Audio Output",
                           "-C", "Dolphin.Input.BackgroundInput=True", "-C", "Logger.Logs.ROLLBACK=True",
                           "-C", "Logger.Logs.NETPLAY=True", "-C", "Logger.Options.Verbosity=3",
                           "-e", args.disc]
                process = subprocess.Popen(command, env=environment, stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE, stderr=stderr, text=True, bufsize=1)
                lines = queue.Queue()
                def read_output(proc=process, output=lines):
                    for line in proc.stdout:
                        output.put(line.rstrip())
                threading.Thread(target=read_output, daemon=True).start()
                players.append((role, process, stderr, lines))
                process.stdin.write("caps join leave stats\n")
                process.stdin.flush()
                # The host's dev ticket must own the first slot before joiners boot.
                if role == 0:
                    deadline = time.monotonic() + 90
                    while time.monotonic() < deadline:
                        with open(Path(work) / "peer-0.log") as log:
                            if "on seat 1" in log.read():
                                break
                        if process.poll() is not None:
                            raise RuntimeError("host exited before owning its room")
                        time.sleep(0.1)
                    else:
                        raise RuntimeError("host never owned its room")
            print(f"Running roles {args.roles}; isolated logs: {work}", flush=True)
            # Operator commands are scoped to one exact harness child, e.g. `3 test-drop-room`.
            def commands():
                for line in sys.stdin:
                    role_text, separator, command = line.rstrip().partition(" ")
                    if separator and role_text.isdigit():
                        for role, process, _, _ in players:
                            if role == int(role_text) and process.poll() is None:
                                try:
                                    process.stdin.write(command + "\n")
                                    process.stdin.flush()
                                except OSError:
                                    pass
            threading.Thread(target=commands, daemon=True).start()
            deadline = time.monotonic() + args.seconds
            expire_at = time.monotonic() + max(15, (args.seconds - 105) / 2) if args.hold_expiry else None
            expired = False
            while time.monotonic() < deadline:
                if expire_at and not expired and time.monotonic() >= expire_at and 3 in roles:
                    process = next(proc for role, proc, _, _ in players if role == 3)
                    process.stdin.write("test-direct off\n")
                    process.stdin.flush()
                    post_fixture(args.site, "/blackout/" + args.room, {"player": "Doubles3"})
                    expired = True
                for role, process, _, lines in players:
                    if args.hold_expiry and role == 3 and expired:
                        continue
                    if process.poll() is not None:
                        raise RuntimeError(f"peer {role} exited")
                    while not lines.empty():
                        line = lines.get()
                        if line.startswith("orca error"):
                            raise RuntimeError(f"peer {role} reported an error; inspect local log")
                        if line.startswith(("orca state", "orca menu")):
                            print(f"peer {role}: {line}", flush=True)
                time.sleep(0.1)
            # All joiners must actually plug in, at the same frame.
            frames = []
            ports = []
            for role, _, _, _ in players:
                if role:
                    text = (Path(work) / f"peer-{role}.log").read_text()
                    assignment = re.search(r"on seat (\d+)", text)
                    match = re.search(r"plugged into port (\d+) at frame (\d+)", text)
                    if (not assignment or not match or int(match[1]) not in (2, 3, 4) or
                            match[1] != assignment[1]):
                        raise RuntimeError(f"peer {role} did not plug into its assigned port")
                    ports.append(int(match[1]))
                    frames.append(int(match[2]))
                    report = {"player": f"Doubles{role}", "port": int(match[1]), "plug": int(match[2])}
                else:
                    report = {"player": "Doubles0", "port": 1, "plug": None}
                post_fixture(args.site, "/report/" + args.room, report)
            if len(set(ports)) != len(ports):
                raise RuntimeError("joiners were assigned duplicate ports")
            if len(set(frames)) > 1:
                raise RuntimeError("joiners plugged in at different frames")
            report_deadline = time.monotonic() + 90
            while True:
                with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                    diagnostics = json.load(response)
                if len(diagnostics["reports"]) == 4:
                    break
                if time.monotonic() > report_deadline:
                    raise RuntimeError("missing cross-machine reports")
                time.sleep(0.2)
            final_frame = verify_diagnostics(diagnostics)
            print(f"PASS: all machines agree on plug {frames}, confirmed hashes through frame {final_frame}")
        finally:
            for _, process, stderr, _ in players:
                if process.poll() is None:
                    try:
                        process.stdin.write("quit\n")
                        process.stdin.flush()
                        process.wait(timeout=10)
                    except (OSError, subprocess.TimeoutExpired):
                        process.kill()  # Only this harness's exact child PID.
                        process.wait()
                stderr.close()
            if args.keep_logs:
                import shutil
                target = Path(args.keep_logs)
                target.mkdir(parents=True, exist_ok=True)
                for log in Path(work).glob("peer-*.log"):
                    shutil.copyfile(log, target / log.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--self-test", action="store_true")
    mode.add_argument("--serve", action="store_true")
    mode.add_argument("--run", action="store_true")
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--site", default="http://127.0.0.1:8765")
    parser.add_argument("--room", default="doubles444")
    parser.add_argument("--roles", default="0,1,2,3")
    parser.add_argument("--binary")
    parser.add_argument("--disc")
    parser.add_argument("--input")
    game = parser.add_mutually_exclusive_group()
    game.add_argument("--profile", help="launcher profile, e.g. PPLUS32")
    game.add_argument("--mod-dir", help="local prepared mod bundle; no download or install")
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--keep-logs")
    parser.add_argument("--turn-binary", help="locally built orca-turn-fixture for --serve or --self-test")
    parser.add_argument("--transport", choices=("direct", "turn"), default="direct")
    parser.add_argument("--hold-expiry", action="store_true", help="expire role 3 halfway through the run")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.turn_binary)
    elif args.serve:
        if not isolated_host(args.listen):
            parser.error("--listen must be loopback or an isolated LAN address")
        server = Server((args.listen, args.port), args.turn_binary)
        print(f"Dev friends-room fixture listening on port {server.server_port}", flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    else:
        if not args.binary or not args.disc:
            parser.error("--run requires --binary and --disc")
        run(args)


if __name__ == "__main__":
    main()
