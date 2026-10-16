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
    def __init__(self, doubles=True):
        self.doubles = doubles
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
        self.away = set()

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
                "offers": list(self.offers),
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
                "room": {"code": room, "queue": "private", "size": 4, "doubles": self.doubles},
                "player": {"id": changed or local},
                "players": [{"id": name, "name": name,
                             **({"away": True} if (room, name) in self.away or (away and name == changed) else {})}
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

    def __init__(self, address, turn_binary=None, doubles=True):
        self.rooms = Rooms(doubles)
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
        if self.path.startswith(("/report/", "/expire/", "/blackout/", "/away/", "/back/")):
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
                elif self.path.startswith("/away/"):
                    rooms.blocked.add((room, player))
                    connection = rooms.sockets.get((room, player))
                    if connection:
                        connection[0].shutdown(socket.SHUT_RDWR)
                elif self.path.startswith("/back/"):
                    rooms.blocked.discard((room, player))
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
        if body.get("dev") != "orca-doubles-test" or player not in {f"Doubles{seat}" for seat in range(5)}:
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
            if (room, player) in rooms.blocked:
                return self.reply({"error": "room-recovering"}, 409)
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
            rooms.away.discard((room, player))
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
                        rooms.away.add((room, player))
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
        try:
            client(site, "Doubles4", "dry444")
            raise AssertionError("fifth client was accepted")
        except urllib.error.HTTPError as error:
            assert error.code == 409
        assert len(server.rooms.members) == 4
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
        # A newcomer/reconnecting peer must see every retained away seat, not only the latest event.
        assert next(player for player in server.rooms.roster("dry444", "Doubles0")["players"]
                    if player["id"] == "Doubles3")["away"]
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
        late = server.rooms.diagnostics("dry444")
        late["reports"]["Doubles2"] = {"port": 3, "plug": 480, "capture": "later-capture"}
        verify_diagnostics(late, expected=3)
        bad = server.rooms.diagnostics("dry444")
        bad["hashes"]["Doubles1"] = {"600": "different"}
        try:
            verify_diagnostics(bad, expected=3)
            raise AssertionError("different final hashes accepted")
        except ValueError:
            pass
        print("PASS: four clients, fifth refused, six pairs, shared capture, retained away roster, "
              "recovery, leave, hold expiry, capture-scoped plugs and confirmed hashes")
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
    batches = {}
    for report in reports.values():
        if report.get("plug") is not None:
            batches.setdefault(report.get("capture", "initial"), set()).add(report["plug"])
    if not batches or any(len(plugs) != 1 for plugs in batches.values()):
        raise ValueError("cross-machine plug frames differ within a capture")
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


def verify_gameplay(work, masks):
    """Require actual human fighter records and a return to CSS for every requested fight."""
    for index, mask in enumerate(masks, 1):
        expected = {port for port in range(4) if mask & (1 << port)}
        for path in Path(work).glob(f"user-*/probe-fight-{index}.bin"):
            record = path.read_bytes()
            humans = {port for port in range(4) if record[port * 0x5c + 1] == 0}
            if humans != expected:
                raise RuntimeError(f"fight {index}: {path.parent.name} humans {humans}, expected {expected}")
        host_probe = Path(work) / "user-0" / f"probe-fight-{index}.bin"
        if not host_probe.exists():
            raise RuntimeError(f"fight {index}: no host fighter record")
    host_log = (Path(work) / "peer-0.log").read_text()
    scenes = re.findall(r"Scene frame (\d+): .* -> (\w+)", host_log)
    fights = [offset for offset, (_, scene) in enumerate(scenes) if scene == "scMelee"]
    if len(fights) < len(masks):
        raise RuntimeError("not all requested fights began")
    for offset in fights[:len(masks)]:
        if not any(scene == "scSelctCharacter" for _, scene in scenes[offset + 1:]):
            raise RuntimeError("a requested fight did not return to character select")


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
    role_inputs = dict(value.split("=", 1) for value in args.input_role)
    role_blackouts = dict(value.split("=", 1) for value in args.blackout_role)
    events = json.loads(Path(args.events).read_text()) if args.events else []
    deferred = set(args.defer_role)
    if not deferred <= set(roles) or any(event.get("role", 0) not in roles for event in events):
        raise ValueError("deferred/event roles must belong to --roles")
    refused = set(args.refused_role)
    refused_seen = set()
    with tempfile.TemporaryDirectory(prefix="orca-doubles-") as work:
        try:
            print(f"Fixture PID: {os.getpid()}", flush=True)
            def spawn(role):
                user = Path(work) / f"user-{role}"
                user.mkdir()
                environment = {"PATH": os.environ.get("PATH", ""),
                               "ORCA_SESSION": "1", "ORCA_TEST_COMMANDS": "1", "ORCA_TEST_ROOM_DROP": "1",
                               "ORCA_TEST_DEV_GAME": "orca-doubles-test", "ORCA_SITE": args.site,
                               "ORCA_ROOM": args.room, "ORCA_JOIN": "0" if role == 0 else "1",
                               "ORCA_TEST_PLAYER": f"Doubles{role}", "ORCA_NAME": f"Doubles{role}",
                               "ORCA_DIRECT": "1", "ORCA_DIRECT_LOOPBACK": "1", "ORCA_DIRECT_STUN": "0"}
                # Headless peers still start the input layer: never let one open a controller or a
                # GameCube adapter that is plugged into this machine (it is somebody's).
                environment.update({"SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_HIDAPI_GAMECUBE": "0",
                                    "SDL_HIDAPI_LIBUSB": "0", "SDL_JOYSTICK_IOKIT": "0", "SDL_JOYSTICK_MFI": "0",
                                    "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0x0000/0x0000"})
                if args.transport == "turn":
                    environment["ORCA_DIRECT_FORCE"] = "turn"
                if os.name == "nt":
                    environment["SystemRoot"] = os.environ.get("SystemRoot", r"C:\Windows")
                environment["YG_SCENES"] = "1"
                if role_blackouts:
                    # The knob adds test=true to the compatibility key. Keep every peer in
                    # that test setup, while only the requested role blacks out during the run.
                    environment["ORCA_TEST_BLACKOUT"] = role_blackouts.get(
                        str(role), "1000000-1000001")
                if args.probe:
                    environment["ORCA_UX_PROBE"] = str(Path(args.probe).resolve())
                input_path = role_inputs.get(str(role), args.input)
                if input_path:
                    environment["YG_INPUT"] = str(Path(input_path).resolve())
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
                def read_output(proc=process, output=lines, role_number=role):
                    with open(Path(work) / f"peer-{role_number}.stdout.log", "w") as log:
                        for line in proc.stdout:
                            log.write(line)
                            log.flush()
                            output.put(line.rstrip())
                threading.Thread(target=read_output, daemon=True).start()
                players.append((role, process, stderr, lines))
                process.stdin.write("caps join leave stats\n")
                process.stdin.flush()
                # Tickets assign seats in arrival order. Wait for every role, since a slow
                # ticket for role 1 must not let role 2 claim its port and invalidate pad scripts.
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    with open(Path(work) / f"peer-{role}.log") as log:
                        assignment = re.search(r"on seat (\d+)", log.read())
                    if assignment:
                        if int(assignment[1]) != role + 1:
                            raise RuntimeError(f"peer {role} owns unexpected seat {assignment[1]}")
                        break
                    if process.poll() is not None:
                        raise RuntimeError(f"peer {role} exited before owning its room seat")
                    time.sleep(0.1)
                else:
                    raise RuntimeError(f"peer {role} never owned its room seat")
            for role in roles:
                if role not in deferred:
                    spawn(role)
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
                for event in events:
                    if event.get("done"):
                        continue
                    if "armed" not in event:
                        source = Path(work) / f"peer-{event.get('source', 0)}.log"
                        text = source.read_text() if source.exists() else ""
                        if len(re.findall(event["pattern"], text)) < event.get("occurrence", 1):
                            continue
                        event["armed"] = time.monotonic()
                    if time.monotonic() < event["armed"] + event.get("delay", 0):
                        continue
                    role = event.get("role", 0)
                    if event["action"] == "spawn":
                        if any(existing == role for existing, _, _, _ in players):
                            raise RuntimeError(f"duplicate scheduled role {role}")
                        spawn(role)
                    elif event["action"] == "command":
                        process = next(proc for number, proc, _, _ in players if number == role)
                        process.stdin.write(event["command"] + "\n")
                        process.stdin.flush()
                    elif event["action"] in ("away", "back"):
                        post_fixture(args.site, f"/{event['action']}/{args.room}", {"player": f"Doubles{role}"})
                    elif event["action"] == "fifth":
                        try:
                            client(args.site, "Doubles4", args.room)
                            raise RuntimeError("fifth arrival was accepted")
                        except urllib.error.HTTPError as error:
                            if error.code != 409:
                                raise
                    else:
                        raise ValueError("unknown fixture event action")
                    event["done"] = True
                    print(f"EVENT: {event}", flush=True)
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
                            if role in refused and line.startswith("orca error room_full"):
                                refused_seen.add(role)
                                print(f"peer {role}: {line}", flush=True)
                                continue
                            raise RuntimeError(f"peer {role} reported an error; inspect local log")
                        if line.startswith("orca stats ") and json.loads(line[11:]).get("ds", 0):
                            raise RuntimeError(f"peer {role} reported a desync")
                        if line.startswith(("orca state", "orca menu")):
                            print(f"peer {role}: {line}", flush=True)
                time.sleep(0.1)
            if any(not event.get("done") for event in events):
                raise RuntimeError("scheduled fixture events did not run")
            if refused_seen != refused:
                raise RuntimeError("expected room_full refusals were not observed")
            # Each capture's joiners must plug in together; later captures have their own frame.
            with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                offers = json.load(response)["offers"]
            frames = []
            ports = []
            for role, _, _, _ in players:
                if role in refused:
                    text = (Path(work) / f"peer-{role}.log").read_text()
                    if "plugged into port" in text:
                        raise RuntimeError("refused newcomer was plugged in")
                    continue
                if role:
                    text = (Path(work) / f"peer-{role}.log").read_text()
                    assignment = re.search(r"on seat (\d+)", text)
                    match = re.search(r"plugged into port (\d+) at frame (\d+)", text)
                    if (not assignment or not match or int(match[1]) not in (2, 3, 4) or
                            match[1] != assignment[1]):
                        raise RuntimeError(f"peer {role} did not plug into its assigned port")
                    ports.append(int(match[1]))
                    frames.append(int(match[2]))
                    capture = next((capture for target, capture, _ in offers if target == f"Doubles{role}"), "initial")
                    report = {"player": f"Doubles{role}", "port": int(match[1]), "plug": int(match[2]), "capture": capture}
                else:
                    report = {"player": "Doubles0", "port": 1, "plug": None}
                post_fixture(args.site, "/report/" + args.room, report)
            if len(set(ports)) != len(ports):
                raise RuntimeError("joiners were assigned duplicate ports")
            report_deadline = time.monotonic() + 90
            while True:
                with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                    diagnostics = json.load(response)
                if len(diagnostics["reports"]) == args.expected:
                    break
                if time.monotonic() > report_deadline:
                    raise RuntimeError("missing cross-machine reports")
                time.sleep(0.2)
            if refused:
                diagnostics["active"] = [name for name in diagnostics["active"]
                                         if name not in {f"Doubles{role}" for role in refused}]
            final_frame = verify_diagnostics(diagnostics, expected=args.expected)
            verify_gameplay(work, args.expect_fight)
            print(f"PASS: all machines agree on plug {frames}, confirmed hashes through frame {final_frame}; "
                  f"0 desyncs; {len(args.expect_fight)} requested fights returned to CSS")
        finally:
            if args.keep_logs:
                target = Path(args.keep_logs)
                target.mkdir(parents=True, exist_ok=True)
                try:
                    with urllib.request.urlopen(args.site + "/diagnostics/" + args.room, timeout=3) as response:
                        (target / "diagnostics.json").write_bytes(response.read())
                except OSError as error:
                    (target / "diagnostics-error.txt").write_text(type(error).__name__ + "\n")
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
                for role in roles:
                    for probe in (Path(work) / f"user-{role}").rglob("probe-*.bin"):
                        shutil.copyfile(probe, target / f"peer-{role}-{probe.name}")


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
    parser.add_argument("--input-role", action="append", default=[], metavar="ROLE=PATH",
                        help="override --input for one role; an empty script keeps it idle")
    parser.add_argument("--blackout-role", action="append", default=[], metavar="ROLE=FROM-TO",
                        help="set ORCA_TEST_BLACKOUT for one role")
    parser.add_argument("--probe", help="read-only gameplay evidence probe script")
    parser.add_argument("--expect-fight", action="append", default=[], type=lambda value: int(value, 0),
                        help="human-port bitmask for each fight; requires probe-fight-N.bin and return to CSS")
    parser.add_argument("--events", help="JSON scheduled per-role spawn/command events, triggered by logs")
    parser.add_argument("--defer-role", action="append", default=[], type=int, choices=range(4))
    parser.add_argument("--refused-role", action="append", default=[], type=int, choices=range(1, 4),
                        help="require room_full for only this newcomer, which must never plug in")
    parser.add_argument("--expected", type=int, default=4, choices=(2, 3, 4))
    parser.add_argument("--doubles", action="store_true", help="accepted for dropin-rooms compatibility")
    parser.add_argument("--ordinary", action="store_true", help="serve an ordinary friends room")
    parser.add_argument("--local", action="store_true", help="own a loopback server for this run")
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
        server = Server((args.listen, args.port), args.turn_binary, doubles=not args.ordinary)
        print(f"Dev friends-room fixture listening on port {server.server_port}", flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    else:
        if not args.binary or not args.disc:
            parser.error("--run requires --binary and --disc")
        server = None
        try:
            if args.local:
                server = Server(("127.0.0.1", 0), args.turn_binary, doubles=not args.ordinary)
                threading.Thread(target=server.serve_forever, daemon=True).start()
                args.site = f"http://127.0.0.1:{server.server_port}"
            run(args)
        finally:
            if server:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
