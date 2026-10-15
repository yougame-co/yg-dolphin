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

    def __init__(self, address):
        self.rooms = Rooms()
        super().__init__(address, Handler)

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
            if key not in members:
                used = {seat for (code, _), seat in members.items() if code == room}
                if len(used) == 4:
                    return self.reply({}, 409)
                members[key] = min(set(range(4)) - used)
        # This literal is a throwaway local fixture value, never a production ticket.
        host = self.headers["Host"]
        self.reply({"url": "http://" + host, "ticket": player, "direct": False})

    def do_GET(self):
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
                    if not departed:
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


def self_test():
    server = Server(("127.0.0.1", 0))
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
        print("PASS: four loopback clients, six pairs, one shared capture, same-seat recovery, isolated leave")
    finally:
        for sock, stream in clients:
            stream.close()
            sock.close()
        server.shutdown()
        server.server_close()


def run(args):
    address = urllib.parse.urlsplit(args.site)
    if address.scheme != "http" or not isolated_host(address.hostname) or address.username or address.password:
        raise ValueError("--site must be a loopback or isolated LAN dev fixture")
    roles = [int(role) for role in args.roles.split(",")]
    if len(set(roles)) != len(roles) or not set(roles) <= set(range(4)):
        raise ValueError("roles must be distinct values from 0..3")
    players = []
    with tempfile.TemporaryDirectory(prefix="orca-doubles-") as work:
        try:
            for role in roles:
                user = Path(work) / f"user-{role}"
                user.mkdir()
                environment = {"HOME": str(user), "PATH": "/usr/bin:/bin:/opt/homebrew/bin",
                               "ORCA_SESSION": "1", "ORCA_TEST_COMMANDS": "1", "ORCA_TEST_ROOM_DROP": "1",
                               "ORCA_TEST_DEV_GAME": "orca-doubles-test", "ORCA_SITE": args.site,
                               "ORCA_ROOM": args.room, "ORCA_JOIN": "0" if role == 0 else "1",
                               "ORCA_TEST_PLAYER": f"Doubles{role}", "ORCA_NAME": f"Doubles{role}"}
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
            while time.monotonic() < deadline:
                for role, process, _, lines in players:
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
            import re
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
            if len(set(ports)) != len(ports):
                raise RuntimeError("joiners were assigned duplicate ports")
            if len(set(frames)) > 1:
                raise RuntimeError("joiners plugged in at different frames")
            print(f"PASS: local joiner ports {ports}, plug frames {frames}")
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
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.serve:
        if not isolated_host(args.listen):
            parser.error("--listen must be loopback or an isolated LAN address")
        server = Server((args.listen, args.port))
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
