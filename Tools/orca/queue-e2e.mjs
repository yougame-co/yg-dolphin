#!/usr/bin/env node
// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
//
// queue-e2e.mjs: two headless Orcas matched by YouGame's real queue (ORCA.md "Matchmaking"), each
// behind an in-process stand-in for the desktop app's bridge that plays the YouGame page's part
// exactly as the page does (web/src/lib/orca-queue.ts, components/OrcaEmbed.tsx, Player.tsx):
//   - answers `orca caps` with `caps join leave stats host results`;
//   - mints Orca's own room tickets (it notes their mode and lobby, as noteOrcaTicket does);
//   - on `orca menu online <q>` mints a queue ticket (orcaQueueRequest: 2 players, the queue's mode,
//     Orca's lobby exactly, `stay` for casual) and opens the rooms Worker's /match;
//   - on `matched`, slot 0 gets `host <code>`, the other `join <code>` 1.5 s later (retried on a soft
//     failure, 3 times 1.5 s apart), and every Orca ticket for that room is minted as the queue's
//     ticket with `match_room` (orcaMatchTicket);
//   - serves the app's controller stream (/controllers/events), whose pad this script presses: the
//     menu path to With Anyone > Casual or Ranked, the character select, the stage, and in the fight
//     port 2 walks off the stage until it has no stocks left.
// It checks the stdout sequence and that both Orcas print the same verdicts (each from its side).
//
//   node Tools/orca/queue-e2e.mjs <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
//       [--queue ranked|casual] [--games <n>] [--site <url>] [--keep] [--alarm <s>]
//       [--slug <listing> --cookie-a <file> --cookie-b <file> --id-a <id> --id-b <id>]
//
// Without --slug: dev tickets (an anonymous dev game, nothing recorded, keyframes in a local folder).
// With --slug: real tickets for that listing as two signed-in players (the cookie files hold a
// session's Cookie header, mode 600, never printed), keyframes through YouGame's store.
// Run rules (Luis's machine): never while he plays (`ps -axo comm= | grep -q
// yougame-desktop/orca/apps`), 1-minute load under 18, under `flock orca-run.lock`; this script
// runs Orca nice'd, muted, Null video, with a perl alarm.
import { spawn } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";

const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : fallback;
};
const [BIN, DISC] = argv;
if (!BIN || !DISC) {
  console.error(
    "usage: queue-e2e.mjs <nogui> <disc> [--pplus <dol>] [--queue ranked|casual] ...",
  );
  process.exit(2);
}
const PPLUS = opt("--pplus");
const QUEUE = opt("--queue", "ranked");
const GAMES = Number(opt("--games", QUEUE === "ranked" ? "3" : "2"));
const SITE = (opt("--site", "https://yougame.co") || "").replace(/\/$/, "");
const SLUG = opt("--slug");
const KEEP = argv.includes("--keep");
const ALARM = Number(opt("--alarm", "900"));
// match (default): the whole flow. cancel: one Orca picks, then backs out of the character select
// while searching (`orca menu cancel`).
const SCENARIO = opt("--scenario", "match");
const PROFILE = PPLUS ? "pplus32" : "rsbe01";
const DEV_GAME = `orca-queue-e2e-${PROFILE}`;
const WORK = fs.mkdtempSync(path.join(os.tmpdir(), "orca-queue-e2e-"));
const T0 = Date.now();
const UA =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";

const log = (text) =>
  console.log(`[${((Date.now() - T0) / 1000).toFixed(1).padStart(6)}] ${text}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = null;
function fail(why) {
  if (!failed) {
    failed = why;
    log(`FAIL: ${why}`);
  }
  throw new Error(why);
}

// GameCube adapter wire mask (desktop/src/controllers.ts).
const BTN = {
  A: 1,
  B: 2,
  X: 4,
  Y: 8,
  LEFT: 16,
  RIGHT: 32,
  DOWN: 64,
  UP: 128,
  START: 256,
  Z: 512,
};

class Side {
  constructor(name, cookieFile, playerId) {
    this.name = name;
    this.cookie = cookieFile ? fs.readFileSync(cookieFile, "utf8").trim() : "";
    this.playerId =
      playerId || `qe2e-${name}-${crypto.randomBytes(6).toString("hex")}`;
    this.token = crypto.randomBytes(16).toString("hex");
    this.lines = [];
    this.waiters = [];
    this.scene = "";
    this.sceneAt = 0;
    this.sceneWaiters = [];
    this.lastOwn = null; // Orca's last own ticket: { mode, lobby }
    this.match = null; // { room, mode, players, lobby, queue }
    this.mirror = null;
    this.ownRooms = [];
    this.pad = { buttons: 0, axes: [128, 128, 128, 128] };
    this.results = [];
  }

  async startBridge() {
    this.server = http.createServer((req, res) => this.serve(req, res));
    await new Promise((r) => this.server.listen(0, "127.0.0.1", r));
    this.port = this.server.address().port;
  }

  serve(req, res) {
    if (req.headers.authorization !== `Bearer ${this.token}`) {
      res.writeHead(401).end();
      return;
    }
    if (req.method === "GET" && req.url === "/events") {
      res.writeHead(200, { "content-type": "text/event-stream" });
      const keep = setInterval(() => res.write(":\n\n"), 15000);
      req.on("close", () => clearInterval(keep));
      return;
    }
    if (req.method === "GET" && req.url === "/controllers/events") {
      res.writeHead(200, { "content-type": "text/event-stream" });
      const session = crypto.randomBytes(4).toString("hex");
      let seq = 0;
      const tick = setInterval(() => {
        const now = Date.now();
        const ports = [0, 1, 2, 3].map((i) => ({
          adapterId: `${session}-gc`,
          port: i,
          seat: i,
          connected: i === 0,
          type: i === 0 ? "wired" : null,
          buttons: i === 0 ? this.pad.buttons : 0,
          axes: i === 0 ? this.pad.axes : [128, 128, 128, 128],
          triggers: [0, 0],
          origin: [128, 128, 128, 128, 0, 0],
          calibrationRevision: 0,
        }));
        const snap = {
          schema: 1,
          session,
          sequence: ++seq,
          sentAt: now,
          receivedAt: now,
          source: "native",
          state: "connected",
          message: "",
          owned: true,
          suspended: false,
          ports,
          pads: [],
        };
        res.write(`data: ${JSON.stringify({ type: "controllers", snapshot: snap })}\n\n`);
      }, 8);
      req.on("close", () => clearInterval(tick));
      return;
    }
    if (req.method === "POST" && req.url === "/sdk") {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", async () => {
        let msg;
        try {
          msg = JSON.parse(body);
        } catch {
          res.writeHead(400).end();
          return;
        }
        let reply;
        try {
          reply = { ok: true, result: await this.sdk(msg) };
        } catch (e) {
          reply = { ok: false, error: String(e.message || e) };
        }
        res
          .writeHead(200, { "content-type": "application/json" })
          .end(JSON.stringify(reply));
      });
      return;
    }
    res.writeHead(404).end();
  }

  async sdk(msg) {
    if (msg.type === "hello") return { room: "" };
    if (msg.type === "mpRoom") {
      this.mirror = msg;
      return {};
    }
    if (msg.type !== "mpTicket") throw new Error(`unknown ${msg.type}`);
    // Player.tsx: Orca's own room's ticket notes its mode and lobby; one for the matched room is
    // the queue's ticket, signed for that room only.
    const m = this.match && msg.room === this.match.room ? this.match : null;
    if (!m && /^orca-[a-z0-9]+$/.test(msg.mode || "")) {
      this.lastOwn = { mode: msg.mode, lobby: msg.lobby };
      if (msg.room && !this.ownRooms.includes(msg.room))
        this.ownRooms.push(msg.room);
    }
    const body = m
      ? {
          players: m.players,
          mode: m.mode,
          lobby: m.lobby,
          match_room: m.room,
          queue: m.queue,
          ...(m.queue === "casual" ? { after_match: "stay" } : {}),
        }
      : {
          players: msg.players,
          mode: msg.mode,
          lobby: msg.lobby,
          queue: "casual",
          room: msg.room,
        };
    const json = await this.ticket(body);
    return {
      url: json.url,
      ticket: json.ticket,
      room: m ? m.room : json.room,
      queue: json.queue,
      rating: json.rating,
      rank: json.rank,
      ice: json.ice,
      direct: json.direct,
      player: { id: json.player?.id },
    };
  }

  async ticket(body) {
    const full = SLUG
      ? { slug: SLUG, player_id: this.playerId, name: this.name, ...body }
      : { dev: DEV_GAME, player_id: this.playerId, name: this.name, ...body };
    const res = await fetch(`${SITE}/api/multiplayer/ticket`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "user-agent": UA,
        ...(this.cookie ? { cookie: this.cookie } : {}),
      },
      body: JSON.stringify(full),
    });
    const json = await res.json().catch(() => ({}));
    if (!res.ok || json.error)
      throw new Error(`ticket ${res.status}: ${json.error || "?"}`);
    return json;
  }

  start() {
    const user = path.join(WORK, `user-${this.name}`);
    fs.mkdirSync(user, { recursive: true });
    const env = {
      HOME: process.env.HOME,
      PATH: process.env.PATH,
      ORCA_SESSION: "1",
      ORCA_TEST_COMMANDS: "1",
      YOUGAME_BRIDGE: `http://127.0.0.1:${this.port}`,
      YOUGAME_TOKEN: this.token,
      YOUGAME_GAME: SLUG || DEV_GAME,
      ORCA_NAME: this.name,
      ORCA_SITE: SITE,
      YG_SCENES: "1",
    };
    if (!SLUG) env.ORCA_TEST_KEYFRAME_DIR = path.join(WORK, "kf");
    if (PPLUS) env.ORCA_PROFILE = "PPLUS32";
    const args = [
      "-n",
      "10",
      "perl",
      "-e",
      `alarm ${ALARM}; exec @ARGV`,
      BIN,
      "-p",
      "headless",
      "-u",
      user,
      "-v",
      "Null",
      "-C",
      "Dolphin.DSP.Backend=No Audio Output",
      "-C",
      "Dolphin.Input.BackgroundInput=True",
      "-C",
      "Logger.Logs.ROLLBACK=True",
      "-C",
      "Logger.Logs.NETPLAY=True",
      "-C",
      "Logger.Options.Verbosity=3",
      "-C",
      "Logger.Options.WriteToConsole=True",
      ...(PPLUS
        ? ["-C", `Dolphin.Core.DefaultISO=${DISC}`, "-e", PPLUS]
        : ["-e", DISC]),
    ];
    this.logPath = path.join(WORK, `${this.name}.log`);
    const logFile = fs.createWriteStream(this.logPath);
    this.proc = spawn("nice", args, { env, stdio: ["pipe", "pipe", "pipe"] });
    let out = "";
    this.proc.stdout.on("data", (d) => {
      out += d;
      let i;
      while ((i = out.indexOf("\n")) >= 0) {
        const line = out.slice(0, i);
        out = out.slice(i + 1);
        this.onLine(line);
      }
    });
    let err = "";
    this.proc.stderr.on("data", (d) => {
      logFile.write(d);
      err += d;
      let i;
      while ((i = err.indexOf("\n")) >= 0) {
        const line = err.slice(0, i);
        err = err.slice(i + 1);
        const s = /Scene frame \d+: \S* -> (\S+)/.exec(line);
        if (s) this.onScene(s[1]);
        if (
          /Matchmaking:|Orca room: (report|room match|game|the set|casual|beginning|a matchmade|\{)/.test(
            line,
          )
        )
          log(
            `${this.name}~ ${line.replace(/^.*?(Matchmaking|Orca room)/, "$1")}`,
          );
      }
    });
    this.proc.on("exit", (code) => {
      this.exited = code ?? -1;
      this.onLine(null);
    });
  }

  onLine(line) {
    if (line !== null) {
      if (line.startsWith("orca stats ")) return;
      if (Buffer.byteLength(line) > 300)
        fail(`${this.name}: a line over 300 bytes: ${line}`);
      log(`${this.name}> ${line}`);
      if (line.startsWith("orca error desync")) fail(`${this.name}: desync`);
      if (line.startsWith("orca result "))
        this.results.push(JSON.parse(line.slice(12)));
    }
    this.lines.push(line);
    for (const w of [...this.waiters]) w();
  }

  onScene(scene) {
    this.scene = scene;
    this.sceneAt = Date.now();
    for (const w of [...this.sceneWaiters]) w();
  }

  send(line) {
    log(`${this.name}< ${line}`);
    this.proc.stdin.write(`${line}\n`);
  }

  // The first line from index `from` on that matches, within `ms`.
  expect(re, ms, from = 0) {
    return new Promise((resolve, reject) => {
      const end = setTimeout(() => {
        cleanup();
        reject(new Error(`${this.name}: no ${re} in ${ms / 1000} s`));
      }, ms);
      const check = () => {
        for (let i = from; i < this.lines.length; ++i) {
          const l = this.lines[i];
          if (l === null) {
            cleanup();
            reject(
              new Error(
                `${this.name} exited (${this.exited}) waiting for ${re}`,
              ),
            );
            return;
          }
          const m = re.exec(l);
          if (m) {
            cleanup();
            resolve({ line: l, index: i, m });
            return;
          }
        }
      };
      const cleanup = () => {
        clearTimeout(end);
        this.waiters = this.waiters.filter((w) => w !== check);
      };
      this.waiters.push(check);
      check();
    });
  }

  waitScene(name, ms) {
    return new Promise((resolve, reject) => {
      const end = setTimeout(() => {
        cleanup();
        reject(
          new Error(`${this.name}: never reached ${name} (at ${this.scene})`),
        );
      }, ms);
      const check = () => {
        if (this.scene === name) {
          cleanup();
          resolve();
        }
      };
      const cleanup = () => {
        clearTimeout(end);
        this.sceneWaiters = this.sceneWaiters.filter((w) => w !== check);
      };
      this.sceneWaiters.push(check);
      check();
    });
  }

  async press(button, ms = 120) {
    this.pad.buttons |= BTN[button];
    await sleep(ms);
    this.pad.buttons &= ~BTN[button];
    await sleep(120);
  }

  async hold(axes, ms) {
    this.pad.axes = axes;
    await sleep(ms);
    this.pad.axes = [128, 128, 128, 128];
    await sleep(120);
  }

  stop() {
    try {
      this.proc.stdin.write("quit\n");
    } catch {}
    setTimeout(() => this.proc.kill("SIGKILL"), 8000).unref();
    this.server.close();
  }
}

// The Online menu's path to With Anyone > Casual or Ranked (Tools/orca/online-menu.py).
async function menuToQueue(side, queue) {
  if (PPLUS) {
    // Project+ boots to its character select: B (held) to the main menu's VERSUS page, B to the top.
    await side.waitScene("scSelctCharacter", 120000);
    await sleep(3000);
    await side.press("B", 900);
    await side.waitScene("muMenuMain", 30000);
    await sleep(2500);
    await side.press("B");
    await sleep(1500);
  } else {
    // Brawl: past the strap, boot and title screens to the main menu (no press once the title
    // leaves: a stray A on the main menu would pick Group).
    const mash = setInterval(() => {
      if (["", "scStrap", "scBoot", "scTitle"].includes(side.scene)) side.press("A");
    }, 700);
    await side.waitScene("muMenuMain", 120000);
    clearInterval(mash);
    await sleep(2500);
  }
  for (const b of [
    "DOWN",
    "A",
    "RIGHT",
    "A",
    ...(queue === "ranked" ? ["DOWN"] : []),
    "A",
  ]) {
    await side.press(b);
    await sleep(700);
  }
}

// The page's queue for one side: a queue ticket, /match, then host or join.
async function queueUp(side, queue, onMatched) {
  const menu = await side.expect(
    /^orca menu (online (casual|ranked|friends)( local)?|cancel)$/,
    60000,
  );
  if (menu.m[3])
    fail(`${side.name}: "${menu.line}": Orca says local with host on`);
  if (menu.m[2] !== queue)
    fail(`${side.name}: picked ${menu.m[2]}, wanted ${queue}`);
  if (!side.lastOwn) fail(`${side.name}: no ticket of Orca's own yet`);
  const mode = `${side.lastOwn.mode}-${queue}`;
  const tk = await side.ticket({
    players: 2,
    mode,
    lobby: side.lastOwn.lobby,
    queue,
    ...(queue === "casual" ? { after_match: "stay" } : {}),
  });
  log(
    `${side.name}: queue ticket in ${mode}${tk.rank ? ` (${tk.rank.label ?? ""})` : ""}`,
  );
  const ws = new WebSocket(
    `${tk.url.replace(/^http/, "ws")}/match?ticket=${encodeURIComponent(tk.ticket)}`,
  );
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`${side.name}: no match in 120 s`)),
      120000,
    );
    ws.onmessage = async (e) => {
      const m = JSON.parse(e.data);
      if (m.t === "searching")
        log(`${side.name}: searching, ${m.waiting} waiting`);
      if (m.t !== "matched") return;
      clearTimeout(timer);
      ws.close(1000, "matched");
      side.match = {
        room: m.room,
        mode,
        players: 2,
        lobby: side.lastOwn.lobby,
        queue,
      };
      log(`${side.name}: matched into ${m.room} as slot ${m.slot}`);
      resolve({ room: m.room, slot: m.slot });
    };
    ws.onerror = () => reject(new Error(`${side.name}: /match failed`));
  });
}

async function hostOrJoin(side, room, slot) {
  if (slot === 0) {
    side.send(`host ${room}`);
    return;
  }
  for (let tries = 0; ; ++tries) {
    await sleep(1500);
    const from = side.lines.length;
    side.send(`join ${room}`);
    const r = await Promise.race([
      side.expect(/^orca state playing$/, 150000, from).then(() => null),
      side
        .expect(/^orca error (network|peer_left) /, 150000, from)
        .then((x) => x.m[1]),
    ]);
    if (r === null) return;
    if (tries >= 3)
      fail(`${side.name}: join failed (${r}) after ${tries + 1} tries`);
    log(`${side.name}: join failed (${r}): retrying`);
  }
}

// One game, driven from the host's scenes (it never loads a keyframe, so its scene lines are
// exact): both pick on the first visit to the character select, the host starts and picks the
// stage, port 2 walks off the stage until the fight ends, both press on at the results.
async function playGame(host, joiner, first) {
  await host.waitScene("scSelctCharacter", 120000);
  await sleep(2500);
  if (first) {
    await Promise.all([host.press("A"), joiner.press("A")]);
    await sleep(600);
    await Promise.all([
      host.hold([128, 255, 128, 128], 700),
      joiner.hold([128, 255, 128, 128], 700),
    ]);
    await sleep(300);
    await Promise.all([host.press("A"), joiner.press("A")]);
    await sleep(1500);
  }
  while (host.scene === "scSelctCharacter") {
    await host.press("START");
    await sleep(1200);
  }
  await host.waitScene("scSelStage", 30000);
  await sleep(1500);
  await host.hold([128, 255, 128, 128], 700);
  await host.press("A");
  for (let i = 0; i < 6 && host.scene === "scSelStage"; ++i) {
    await sleep(1500);
    await host.press("A");
  }
  await host.waitScene("scMelee", 30000);
  await sleep(6000);
  // Port 2 walks off the left edge, again and again.
  joiner.pad.axes = [0, 128, 128, 128];
  const hostFrom = host.lines.length;
  await host
    .waitScene("scVsResult", 240000)
    .finally(() => (joiner.pad.axes = [128, 128, 128, 128]));
  await sleep(4000);
  const presser = setInterval(() => {
    if (host.scene !== "scVsResult") return;
    host.press(Date.now() % 2 ? "A" : "START");
    joiner.press("A");
  }, 900);
  try {
    await Promise.race([
      host.waitScene("scSelctCharacter", 60000),
      host.expect(/^orca state friend-left match-over$/, 60000, hostFrom),
    ]);
  } finally {
    clearInterval(presser);
  }
}

function mirrored(a, b) {
  // The same verdict seen from both sides: n, out opposite (or both void/draw), d mirrored.
  if (a.k !== b.k || a.q !== b.q || a.n !== b.n) return false;
  const opp = { won: "lost", lost: "won", draw: "draw", void: "void" };
  if (opp[a.out] !== b.out) return false;
  if (
    a.score &&
    b.score &&
    (a.score[0] !== b.score[1] || a.score[1] !== b.score[0])
  )
    return false;
  if (a.d && b.d) {
    if (a.d.st !== b.d.st || a.d.to !== b.d.to) return false;
    if (a.d.c[0] !== b.d.c[1] || a.d.c[1] !== b.d.c[0]) return false;
    if (a.d.s[0] !== b.d.s[1] || a.d.s[1] !== b.d.s[0]) return false;
  }
  return true;
}

async function cancelScenario() {
  const a = new Side("ada", opt("--cookie-a"), opt("--id-a"));
  sides.push(a);
  await a.startBridge();
  a.start();
  await a.expect(/^orca caps .*\bhost\b/, 120000);
  a.send("caps join leave stats pause delay perf direct host results");
  await menuToQueue(a, QUEUE);
  const menu = await a.expect(/^orca menu online (casual|ranked)( local)?$/, 60000);
  if (menu.m[2]) fail(`"${menu.line}": local with host on`);
  await a.waitScene("scSelctCharacter", 30000);
  await sleep(3000);
  // B held out of the character select: back on the menu, the search is over.
  const from = a.lines.length;
  await a.press("B", 1500);
  await a.expect(/^orca menu cancel$/, 30000, from);
  log("PASS: backing out of the character select while searching printed orca menu cancel");
}

const sides = [];
async function main() {
  if (SCENARIO === "cancel") return cancelScenario();
  const a = new Side("ada", opt("--cookie-a"), opt("--id-a"));
  const b = new Side("bo", opt("--cookie-b"), opt("--id-b"));
  sides.push(a, b);
  for (const s of [a, b]) await s.startBridge();
  log(
    `work ${WORK}; ${SLUG ? `listing ${SLUG} on ${SITE}` : `dev game ${DEV_GAME}`}; ${QUEUE}`,
  );
  a.start();
  await sleep(4000);
  b.start();
  for (const s of [a, b]) {
    await s.expect(/^orca caps .*\bhost\b/, 120000);
    if (
      QUEUE === "ranked" &&
      !s.lines.some((l) => l && /^orca caps .*\bresults\b/.test(l))
    )
      fail(`${s.name}: Orca offers no results for this profile`);
    s.send("caps join leave stats pause delay perf direct host results");
  }
  // Ada picks first, so she waits longest and gets slot 0 (the host).
  const queued = [];
  await menuToQueue(a, QUEUE);
  queued.push(queueUp(a, QUEUE));
  await sleep(6000);
  await menuToQueue(b, QUEUE);
  queued.push(queueUp(b, QUEUE));
  const [ma, mb] = await Promise.all(queued);
  if (ma.room !== mb.room)
    fail(`matched into different rooms: ${ma.room} ${mb.room}`);
  const host = ma.slot === 0 ? a : b;
  const joiner = host === a ? b : a;
  log(`host ${host.name}, joiner ${joiner.name}, room ${ma.room}`);
  const hostFrom = host.lines.length;
  await Promise.all([
    hostOrJoin(a, ma.room, ma.slot),
    hostOrJoin(b, mb.room, mb.slot),
  ]);
  await host.expect(/^orca state friend-joined$/, 120000, hostFrom);
  if (host.lines.slice(hostFrom).some((l) => l === "unsupported host"))
    fail("unsupported host");

  const wantGames = QUEUE === "ranked" ? 2 : GAMES;
  for (let g = 0; g < GAMES; ++g) {
    await playGame(host, joiner, g === 0);
    const games = host.results.filter((r) => r.k === "game").length;
    log(`after game ${g + 1}: host has ${games} game line(s)`);
    if (QUEUE === "ranked" && host.results.some((r) => r.k === "set")) break;
    if (QUEUE === "casual" && games >= wantGames) break;
  }
  if (QUEUE === "ranked") {
    await host.expect(/^orca result \{"q":"ranked","k":"set"/, 60000);
    await joiner.expect(/^orca result \{"q":"ranked","k":"set"/, 60000);
    await host.expect(/^orca state friend-left match-over$/, 30000);
    await joiner.expect(/^orca state host-left match-over$/, 30000);
    // The host's next room is a fresh one of its own, never the closed code.
    await sleep(4000);
    const fresh = host.ownRooms.filter((r) => r !== ma.room);
    if (!fresh.length) fail("the host opened no room of its own after the set");
  } else {
    const from = host.lines.length;
    joiner.send("leave");
    await host.expect(/^orca state friend-left /, 60000, from);
  }

  // Both printed the same verdicts, each from its own side.
  const ga = host.results,
    gb = joiner.results;
  if (ga.length !== gb.length)
    fail(`${ga.length} result lines on the host, ${gb.length} on the joiner`);
  for (let i = 0; i < ga.length; ++i)
    if (!mirrored(ga[i], gb[i]))
      fail(
        `result ${i + 1} differs: ${JSON.stringify(ga[i])} / ${JSON.stringify(gb[i])}`,
      );
  for (const s of [a, b]) {
    const errors = s.lines.filter((l) => l && l.startsWith("orca error "));
    if (errors.length) fail(`${s.name}: ${errors.join("; ")}`);
  }
  log(`PASS: ${ga.length} verdicts, the same on both sides`);
}

let code = 0;
try {
  await main();
} catch (e) {
  if (!failed) log(`FAIL: ${e.message}`);
  code = 1;
} finally {
  for (const s of sides) if (s.proc) s.stop();
}
// Both Orcas stop (quit, then killed after 8 s), then the work folder goes unless kept.
await sleep(9000);
if (!KEEP && code === 0) fs.rmSync(WORK, { recursive: true, force: true });
else log(`logs in ${WORK}`);
process.exit(code);
