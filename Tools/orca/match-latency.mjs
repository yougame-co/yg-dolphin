#!/usr/bin/env node
// match-latency.mjs: how long from "matched" to both players in control on the room's character
// select (ORCA.md, "Match start timing"). Two headless Orcas meet on YouGame exactly as the desktop
// app's page drives them (the in-process bridge of queue-e2e.mjs: Orca's own tickets, the queue
// ticket, /match, `host`/`join` at once as lib/orca-queue.ts ORCA_JOIN_DELAY_MS = 0, the matched
// room's ticket on `mpTicket`, `queue rating -`; friends: `prepare-join`, then `join`), N matches in
// a row, and every line either side prints is recorded with its arrival time (and Orca's own
// mm:ss:mmm stamp), so match-latency-report.mjs can cut each match into phases afterwards. Nothing
// is judged here: the raw timeline is the product.
//
//   both sides on this machine:
//     node match-latency.mjs --pair --mode casual|ranked|friends --bin <Orca> --disc <disc>
//       [--pplus <dol>] --slug <listing> --a <name> --b <name> --sess <dir>
//       --orca-build <version>:<commit> --matches 10 --out <run dir>
//       [--video Null|Metal] [--shots <ms>] [--hold <ms>] [--gap <s>] [--alarm <s>] [--site <url>]
//   one side (Mac<->PC: run one on each machine with the same --search-at):
//     node match-latency.mjs --side a|b --mode casual ... --a <name> --search-at <unix s>
//       [--search-delay <ms>] [--friend-room <code>]
//
// --sess <dir> holds <name>.cookie (a session Cookie header for that /sandbox roster account, mode
// 600; match-latency-session.mjs makes them) and ids.json. Only ever roster accounts: the matches
// are real queue matches on the site. --orca-build is the page's word on the Orca that runs (the
// ticket route only lets the current build search); a candidate build meets only another copy of
// itself anyway, since Orca's lobby key carries its commit.
// --gap <s>: idle this long before every search after the first, so the rooms Worker's isolates
// and the site's caches go cold between matches, as between real players' matches.
//
// Environment: SDL_* and ORCA_TEST_* pass through to Orca. Every run must carry a test mark
// (ORCA_TEST_NET_DELAY_MS, 0 adds no delay) so the test Orcas only ever meet each other.
//
// Output (--out): <name>.raw.jsonl per side ({t, k, ...}: k "o" Orca stdout, "e" Orca stderr, "b"
// a bridge/page event), <name>.orca.log (Orca's whole stderr), meta.json (args, versions, the
// clock's offset to Cloudflare's edge before and after), shots/ with --shots.
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
const has = (name) => argv.includes(name);
const MODE = opt("--mode", "casual");
const PAIR = has("--pair");
const ONE = opt("--side"); // a | b
const BIN = opt("--bin");
const BIN_B = opt("--bin-b", BIN);
const DISC = opt("--disc");
const PPLUS = opt("--pplus");
const SLUG = opt("--slug");
const SITE = (opt("--site", "https://yougame.co") || "").replace(/\/$/, "");
const SESS = opt("--sess");
const NAME_A = opt("--a", "cy");
const NAME_B = opt("--b", "di");
const MATCHES = Number(opt("--matches", "10"));
const OUT = opt("--out");
const VIDEO = opt("--video", "Null");
const SHOTS_MS = Number(opt("--shots", "0"));
const HOLD_MS = Number(opt("--hold", "2500"));
const ALARM = Number(opt("--alarm", "1500"));
const SEARCH_AT = Number(opt("--search-at", "0")) * 1000;
const FRIEND_ROOM = opt("--friend-room");
const FRIEND_IDLE_MS = Number(opt("--friend-idle", "13000"));
const DELAY = opt("--delay", "3");
// --search-delay <ms>: wait this long before each search (Mac<->PC: the other side then waits longest and hosts).
const SEARCH_DELAY = Number(opt("--search-delay", "0"));
// --gap <s>: idle before every search after the first (cold isolates and caches between matches).
const GAP_MS = Number(opt("--gap", "0")) * 1000;
const WIN = process.platform === "win32";
const QUEUE = MODE === "friends" ? null : MODE;
if (
  !BIN ||
  !DISC ||
  !SLUG ||
  !SESS ||
  !OUT ||
  !opt("--orca-build") ||
  (!PAIR && !ONE)
) {
  console.error(
    "usage: match-latency.mjs --pair|--side a|b --mode casual|ranked|friends --bin --disc --slug --sess --orca-build --out ...",
  );
  process.exit(2);
}
if (!["casual", "ranked", "friends"].includes(MODE)) {
  console.error("--mode casual|ranked|friends");
  process.exit(2);
}
if (!process.env.ORCA_TEST_NET_DELAY_MS) {
  console.error(
    "refusing to run without a test mark: set ORCA_TEST_NET_DELAY_MS (0 is fine)",
  );
  process.exit(2);
}
const [obVersion, obCommit] = opt("--orca-build").split(":");
const ORCA_BUILD = { version: obVersion, commit: obCommit || null };
fs.mkdirSync(OUT, { recursive: true });
const WORK = fs.mkdtempSync(path.join(os.tmpdir(), "orca-match-latency-"));
const T0 = Date.now();
const UA =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
const log = (text) =>
  console.log(`[${((Date.now() - T0) / 1000).toFixed(1).padStart(7)}] ${text}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
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
  R: 1024,
  L: 2048,
};
// What the page answers to `orca caps` (lib/orca-protocol.ts): no pick caps, as queue-e2e.
const CAPS =
  "caps join leave stats pause delay perf direct host results queue2";
const ids = JSON.parse(fs.readFileSync(path.join(SESS, "ids.json"), "utf8"));

// The clock's offset to Cloudflare's edge (its /cdn-cgi/trace `ts`, ms precision): the sample with
// the shortest round trip, offset = edge - midpoint. Mac and PC both measure it, so their events
// can be put on one clock.
async function edgeOffset(n = 7) {
  let best = null;
  for (let i = 0; i < n; ++i) {
    const t1 = Date.now();
    try {
      const r = await fetch(`${SITE}/cdn-cgi/trace?${i}`, {
        headers: { "user-agent": UA },
      });
      const text = await r.text();
      const t2 = Date.now();
      const ts = /ts=([\d.]+)/.exec(text);
      const colo = /colo=(\w+)/.exec(text)?.[1];
      if (ts) {
        const rtt = t2 - t1;
        const off = Number(ts[1]) * 1000 - (t1 + t2) / 2;
        if (!best || rtt < best.rtt) best = { rtt, off: Math.round(off), colo };
      }
    } catch {}
    await sleep(150);
  }
  return best;
}

class Side {
  constructor(key, name) {
    this.key = key; // a | b
    this.name = name;
    this.cookie = fs
      .readFileSync(path.join(SESS, `${name}.cookie`), "utf8")
      .trim();
    this.playerId =
      ids[name]?.id || `ml-${name}-${crypto.randomBytes(4).toString("hex")}`;
    this.token = crypto.randomBytes(16).toString("hex");
    this.lines = []; // Orca stdout protocol lines (null = exited)
    this.err = []; // { t, line } stderr lines
    this.waiters = [];
    this.scene = "";
    this.sceneWaiters = [];
    this.lastOwn = null;
    this.match = null;
    this.pad = { buttons: 0, axes: [128, 128, 128, 128] };
    this.raw = fs.createWriteStream(path.join(OUT, `${name}.raw.jsonl`));
    this.ownRoom = null;
    this.shotSeq = 0;
  }

  rec(k, obj) {
    this.raw.write(JSON.stringify({ t: Date.now(), k, ...obj }) + "\n");
  }
  ev(ev, data = {}) {
    this.rec("b", { ev, ...data });
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
          triggers:
            i === 0
              ? [
                  this.pad.buttons & BTN.L ? 255 : 0,
                  this.pad.buttons & BTN.R ? 255 : 0,
                ]
              : [0, 0],
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
        res.write(
          `data: ${JSON.stringify({ type: "controllers", snapshot: snap })}\n\n`,
        );
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
      this.ev("mpRoom", {
        room: msg.room ?? msg.code ?? null,
        players: Array.isArray(msg.players) ? msg.players.length : undefined,
      });
      return {};
    }
    if (msg.type !== "mpTicket") throw new Error(`unknown ${msg.type}`);
    // As Player.tsx: Orca's own room gives the queue its mode and key; the matched room's ticket is
    // the queue's, signed for that room only (match_room).
    const m = this.match && msg.room === this.match.room ? this.match : null;
    if (!m && /^orca-[a-z0-9]+$/.test(msg.mode || ""))
      this.lastOwn = { mode: msg.mode, lobby: msg.lobby };
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
    this.ev("ticket_req", { room: msg.room ?? null, matched: !!m });
    const json = await this.ticket(body);
    if (!m && json.room) this.ownRoom = json.room;
    this.ev("ticket_ok", {
      room: m ? m.room : json.room,
      matched: !!m,
      direct: json.direct,
      ice: !!json.ice,
    });
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
    const full = {
      slug: SLUG,
      player_id: this.playerId,
      name: this.name[0].toUpperCase() + this.name.slice(1),
      orca_build: ORCA_BUILD,
      ...body,
    };
    const t1 = Date.now();
    const res = await fetch(`${SITE}/api/multiplayer/ticket`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "user-agent": UA,
        cookie: this.cookie,
      },
      body: JSON.stringify(full),
    });
    const json = await res.json().catch(() => ({}));
    this.ev("ticket_http", {
      ms: Date.now() - t1,
      status: res.status,
      kind: body.match_room ? "match_room" : body.room ? "own" : "queue",
    });
    if (!res.ok || json.error)
      throw new Error(
        `ticket ${res.status}: ${json.error || "?"} ${json.code || ""}`,
      );
    return json;
  }

  start(bin) {
    const user = path.join(WORK, `user-${this.name}`);
    fs.mkdirSync(user, { recursive: true });
    this.user = user;
    const env = {};
    for (const k of [
      "HOME",
      "PATH",
      "SYSTEMROOT",
      "SystemRoot",
      "TEMP",
      "TMP",
      "USERPROFILE",
      "APPDATA",
      "LOCALAPPDATA",
    ])
      if (process.env[k] !== undefined) env[k] = process.env[k];
    Object.assign(env, {
      ORCA_SESSION: "1",
      ORCA_TEST_COMMANDS: "1",
      YOUGAME_BRIDGE: `http://127.0.0.1:${this.port}`,
      YOUGAME_TOKEN: this.token,
      YOUGAME_GAME: SLUG,
      ORCA_NAME: this.name[0].toUpperCase() + this.name.slice(1),
      ORCA_SITE: SITE,
      YG_SCENES: "1",
    });
    for (const [k, v] of Object.entries(process.env))
      if ((k.startsWith("ORCA_TEST_") || k.startsWith("SDL_")) && !(k in env))
        env[k] = v;
    if (PPLUS) env.ORCA_PROFILE = "PPLUS32";
    if (MODE === "friends" && this.friendHost && this.friendRoom)
      env.ORCA_ROOM = this.friendRoom;
    const video = VIDEO;
    if (video !== "Null") env.ORCA_TEST_PRESENT = "640x528";
    const orcaArgs = [
      "-p",
      "headless",
      "-u",
      user,
      "-v",
      video,
      "-C",
      "Dolphin.DSP.Backend=No Audio Output",
      "-C",
      "Dolphin.Input.BackgroundInput=True",
      "-C",
      "Logger.Logs.ROLLBACK=True",
      "-C",
      "Logger.Logs.NETPLAY=True",
      "-C",
      "Logger.Logs.CONTROLLERINTERFACE=True",
      "-C",
      "Logger.Options.Verbosity=3",
      "-C",
      "Logger.Options.WriteToConsole=True",
      ...(video !== "Null"
        ? ["-C", "Graphics.Settings.LogRenderTimeToFile=True"]
        : []),
      ...(SHOTS_MS
        ? ["-C", "Dolphin.Interface.OnScreenDisplayMessages=False"]
        : []),
      ...(PPLUS
        ? ["-C", `Dolphin.Core.DefaultISO=${DISC}`, "-e", PPLUS]
        : ["-e", DISC]),
    ];
    this.logFile = fs.createWriteStream(
      path.join(OUT, `${this.name}.orca.log`),
    );
    if (WIN) {
      this.proc = spawn(bin, orcaArgs, {
        env,
        stdio: ["pipe", "pipe", "pipe"],
        windowsHide: true,
      });
      this.alarm = setTimeout(() => this.proc.kill(), ALARM * 1000);
      this.alarm.unref();
    } else {
      this.proc = spawn(
        "nice",
        [
          "-n",
          "10",
          "perl",
          "-e",
          `alarm ${ALARM}; exec @ARGV`,
          bin,
          ...orcaArgs,
        ],
        { env, stdio: ["pipe", "pipe", "pipe"] },
      );
    }
    this.ev("orca_spawn", {
      video,
      bin: path.basename(path.dirname(path.dirname(bin))),
    });
    const pipe = (stream, k) => {
      let buf = "";
      stream.on("data", (d) => {
        if (k === "e") this.logFile.write(d);
        buf += d;
        let i;
        while ((i = buf.indexOf("\n")) >= 0) {
          const line = buf.slice(0, i).replace(/\r$/, "");
          buf = buf.slice(i + 1);
          if (k === "o") this.onOut(line);
          else this.onErr(line);
        }
      });
    };
    pipe(this.proc.stdout, "o");
    pipe(this.proc.stderr, "e");
    this.proc.on("exit", (code) => {
      this.exited = code ?? -1;
      this.ev("orca_exit", { code: this.exited });
      this.lines.push(null);
      for (const w of [...this.waiters]) w();
    });
  }

  onOut(line) {
    // Windows Orca writes its log to stdout: a stamped log line is a log line, wherever it came from.
    if (/^\d\d:\d\d:\d\d\d /.test(line)) {
      this.logFile.write(line + "\n");
      return this.onErr(line);
    }
    // Orca's stdout: the protocol (orca ..., ready, unsupported ...). Stats lines are kept too:
    // they say which transport carried the session (tx) and the link's round trip.
    this.rec("o", { s: line });
    if (!line.startsWith("orca stats "))
      log(`${this.name}> ${line.slice(0, 200)}`);
    this.lines.push(line);
    for (const w of [...this.waiters]) w();
  }

  onErr(line) {
    this.rec("e", { s: line });
    const now = Date.now();
    this.err.push({ t: now, line });
    const s = /Scene frame \d+: \S* -> (\S+)/.exec(line);
    if (s) {
      this.scene = s[1];
      for (const w of [...this.sceneWaiters]) w();
    }
    if (
      /Drop-in: |Matchmaking: |Orca room: (in room|a matchmade)|Queue: (frame \d+: port \d's (own )?pick|back on)|Orca direct link: seat \d up|Desync at frame/.test(
        line,
      )
    )
      log(`${this.name}~ ${line.replace(/^.*?: (?=[A-Z])/, "").slice(0, 200)}`);
    for (const w of [...this.waiters]) w();
  }

  send(line) {
    const outs = [line];
    if (line.startsWith("caps") && /\bdelay\b/.test(line))
      outs.push(`delay ${DELAY}`);
    for (const out of outs) {
      this.ev("cmd", { s: out });
      log(`${this.name}< ${out}`);
      this.proc.stdin.write(`${out}\n`);
    }
  }

  // Waits for a stdout line (from index `from`) or a stderr line (errFrom) matching re.
  expect(re, ms, from = 0) {
    return this.waitOn(
      () => {
        for (let i = from; i < this.lines.length; ++i) {
          const l = this.lines[i];
          if (l === null)
            throw new Error(
              `${this.name} exited (${this.exited}) waiting for ${re}`,
            );
          const m = re.exec(l);
          if (m) return { line: l, index: i, m };
        }
        return null;
      },
      ms,
      `${this.name}: no ${re} in ${ms / 1000} s`,
    );
  }
  expectErr(re, ms, from = 0) {
    return this.waitOn(
      () => {
        if (this.exited !== undefined)
          throw new Error(
            `${this.name} exited (${this.exited}) waiting for ${re}`,
          );
        for (let i = from; i < this.err.length; ++i) {
          const m = re.exec(this.err[i].line);
          if (m)
            return { line: this.err[i].line, index: i, m, t: this.err[i].t };
        }
        return null;
      },
      ms,
      `${this.name}: no log ${re} in ${ms / 1000} s`,
    );
  }
  waitOn(check, ms, why) {
    return new Promise((resolve, reject) => {
      const end = setTimeout(() => {
        cleanup();
        reject(new Error(why));
      }, ms);
      const run = () => {
        let r;
        try {
          r = check();
        } catch (e) {
          cleanup();
          reject(e);
          return;
        }
        if (r) {
          cleanup();
          resolve(r);
        }
      };
      const cleanup = () => {
        clearTimeout(end);
        this.waiters = this.waiters.filter((w) => w !== run);
      };
      this.waiters.push(run);
      run();
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

  // --shots: a screenshot request every SHOTS_MS from now until stopShots(); each lands at Orca's
  // next present, so the files' times show what was on screen and when nothing was.
  startShots(tag) {
    if (!SHOTS_MS || VIDEO === "Null") return;
    this.stopShots();
    this.shotTimer = setInterval(() => {
      const name = `${tag}-${String(++this.shotSeq).padStart(4, "0")}`;
      this.ev("shot_req", { name });
      try {
        this.proc.stdin.write(`test-shot ${name}\n`);
      } catch {}
    }, SHOTS_MS);
  }
  stopShots() {
    if (this.shotTimer) clearInterval(this.shotTimer);
    this.shotTimer = null;
  }

  stop() {
    this.stopShots();
    try {
      this.proc.stdin.write("quit\n");
    } catch {}
    setTimeout(() => {
      try {
        this.proc.kill(WIN ? undefined : "SIGKILL");
      } catch {}
    }, 8000).unref();
    this.server.close();
  }

  collect() {
    // Orca's present intervals (Graphics.Settings.LogRenderTimeToFile) and the screenshots.
    const logs = path.join(this.user, "Logs");
    for (const f of ["render_times.txt", "vblank_times.txt"]) {
      const p = path.join(logs, f);
      if (fs.existsSync(p))
        fs.copyFileSync(p, path.join(OUT, `${this.name}.${f}`));
    }
    const shots = path.join(this.user, "ScreenShots");
    if (fs.existsSync(shots)) {
      const to = path.join(OUT, "shots", this.name);
      fs.mkdirSync(to, { recursive: true });
      const list = [];
      for (const f of fs.readdirSync(shots)) {
        const p = path.join(shots, f);
        const st = fs.statSync(p);
        fs.copyFileSync(p, path.join(to, f));
        list.push({ f, mtime: st.mtimeMs, birth: st.birthtimeMs });
      }
      fs.writeFileSync(path.join(to, "index.json"), JSON.stringify(list));
    }
  }
}

// Mash past the strap and title, then the main menu (Tools/orca/online-menu.py's paths).
async function toMainMenu(side) {
  const mash = setInterval(() => {
    if (["", "scStrap", "scBoot", "scTitle"].includes(side.scene))
      side.press("A");
  }, 700);
  await side.waitScene("muMenuMain", 180000).finally(() => clearInterval(mash));
  await sleep(2500);
}

async function menuToQueue(side, queue) {
  await toMainMenu(side);
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

// On the queue's own character select: a pick (up into the grid, A) and Start until Orca says ready.
async function q2Ready(side, queue) {
  await side.waitScene("scSelctCharacter", 60000);
  await sleep(2500);
  await side.hold([128, 255, 128, 128], 450);
  await sleep(300);
  await side.press("A");
  await sleep(1500);
  for (let tries = 0; ; ++tries) {
    const at = side.lines.length;
    await side.press("START");
    const r = await side
      .expect(/^orca queue ready (casual|ranked) (\d+) (\d+)$/, 4000, at)
      .catch(() => null);
    if (r) {
      if (r.m[1] !== queue)
        throw new Error(`${side.name}: ${r.line}, wanted ${queue}`);
      return r;
    }
    if (tries >= 4)
      throw new Error(
        `${side.name}: Start on its own character select never readied it`,
      );
  }
}

// The page's search (OrcaEmbed startSearch): a queue ticket in Orca's own mode and lobby, /match.
async function search(side, queue, n) {
  if (!side.lastOwn)
    throw new Error(`${side.name}: no ticket of Orca's own yet`);
  const mode = `${side.lastOwn.mode}-${queue}`;
  side.ev("search", { n, mode });
  const tk = await side.ticket({
    players: 2,
    mode,
    lobby: side.lastOwn.lobby,
    queue,
    ...(queue === "casual" ? { after_match: "stay" } : {}),
  });
  const ws = new WebSocket(
    `${tk.url.replace(/^http/, "ws")}/match?ticket=${encodeURIComponent(tk.ticket)}`,
  );
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      try {
        ws.close(1000, "timeout");
      } catch {}
      reject(new Error(`${side.name}: no match in 150 s`));
    }, 150000);
    ws.onopen = () => side.ev("match_ws_open", { n });
    ws.onclose = (e) => {
      clearTimeout(timer);
      reject(new Error(`${side.name}: /match closed (${e.code} ${e.reason})`));
    };
    ws.onmessage = (e) => {
      let m;
      try {
        m = JSON.parse(e.data);
      } catch {
        return;
      }
      if (m.t === "searching") side.ev("searching", { waiting: m.waiting });
      if (m.t !== "matched") return;
      // The moment the page learns of the match (t = 0 of every phase).
      side.ev("matched", { n, room: m.room, slot: m.slot });
      clearTimeout(timer);
      ws.onclose = null;
      ws.close(1000, "matched");
      side.match = {
        room: m.room,
        mode,
        players: 2,
        lobby: side.lastOwn.lobby,
        queue,
      };
      log(`${side.name}: match ${n} into ${m.room} as slot ${m.slot}`);
      resolve({ room: m.room, slot: m.slot });
    };
    ws.onerror = () => reject(new Error(`${side.name}: /match failed`));
  });
}

// The page's matchedInto: slot 0 hosts, the other joins at once; a join refused because the host's
// room isn't up yet is tried again 1.5 s later (ORCA_JOIN_RETRY_MS), up to 3 more times.
async function hostOrJoin(side, room, slot, queue) {
  side.send(`queue rating -`);
  if (slot === 0) {
    side.send(`host ${room}`);
    return;
  }
  for (let tries = 0; ; ++tries) {
    if (tries) await sleep(1500);
    const from = side.lines.length;
    side.send(`join ${room}`);
    const r = await Promise.race([
      side.expect(/^orca state playing$/, 60000, from).then(() => null),
      side
        .expect(/^orca error (network|peer_left) /, 60000, from)
        .then((x) => x.m[1]),
    ]);
    if (r === null) return;
    side.ev("join_retry", { why: r, tries });
    if (tries >= 3)
      throw new Error(
        `${side.name}: join failed (${r}) after ${tries + 1} tries`,
      );
  }
}

// One side's queue loop: N matches, each measured from `matched` until both picks are in on this
// side's game (the host's own pick back, the joiner's put in), held HOLD_MS, then the joiner leaves
// and both go back to their own select, ready again, and search again.
async function queueLoop(side, queue, results) {
  await side.expect(/^orca caps .*\bqueue2\b/, 180000);
  side.send(CAPS);
  const from = side.lines.length;
  await menuToQueue(side, queue);
  await side.expect(new RegExp(`^orca menu online ${queue}$`), 60000, from);
  await sleep(1500);
  if (SEARCH_AT) {
    const wait = SEARCH_AT - 6000 - Date.now();
    if (wait > 0) await sleep(wait);
  }
  let readyFrom = side.lines.length;
  await q2Ready(side, queue);
  if (SEARCH_AT) {
    const left = SEARCH_AT - Date.now();
    if (left > 0) await sleep(left);
  }
  for (let n = 1; n <= MATCHES; ++n) {
    const r = { n, side: side.name, ok: false };
    results.push(r);
    try {
      if (GAP_MS && n > 1) await sleep(GAP_MS);
      if (SEARCH_DELAY && n > 1) await sleep(SEARCH_DELAY);
      const m = await search(side, queue, n);
      r.room = m.room;
      r.role = m.slot === 0 ? "host" : "join";
      const errFrom = side.err.length;
      const outFrom = side.lines.length;
      side.startShots(`m${n}`);
      await hostOrJoin(side, m.room, m.slot, queue);
      // This side's end point: port 2's pick is in (the host also needs its own pick back first).
      await side.expectErr(
        /Queue: frame \d+: port 2's pick is in/,
        60000,
        errFrom,
      );
      if (r.role === "host")
        await side.expectErr(
          /Drop-in: port 1's pick is back at frame/,
          5000,
          errFrom,
        );
      side.ev("endpoint", { n });
      r.ok = true;
      await sleep(HOLD_MS);
      side.stopShots();
      // The joiner leaves (the page's Leave); the host goes back alone. Both: their own select again.
      const backFrom = side.err.length;
      const readyAt = side.lines.length;
      if (r.role === "join") side.send("leave");
      await side.expectErr(
        /Queue: back on this player's own character select/,
        60000,
        backFrom,
      );
      const rd = await side
        .expect(new RegExp(`^orca queue ready ${queue} `), 6000, readyAt)
        .catch(() => null);
      if (!rd) {
        side.ev("reready_press");
        await q2Ready(side, queue);
      }
      if (errFrom < 0 || outFrom < 0) throw new Error("unreachable");
    } catch (e) {
      r.error = String(e.message || e);
      side.ev("match_error", { n, error: r.error });
      log(`${side.name}: match ${n} failed: ${r.error}`);
      side.stopShots();
      // Back to a clean state: leave whatever room, then pick again on our own select.
      side.send("leave");
      await sleep(4000);
      if (side.scene === "scSelctCharacter")
        await q2Ready(side, queue).catch(() => {});
      else throw e;
    }
  }
  readyFrom = 0;
  return readyFrom;
}

// Friends: the host idles on its main menu (its own room ORCA_ROOM from the start, as an invite
// link names it); the friend's page sends `join <code>` (an accepted invite) after the inviter's
// `prepare-join`. Measured from the friend's join until both are in the game.
async function friendsHost(side, peerDone) {
  await side.expect(/^orca caps /, 180000);
  side.send(CAPS);
  await toMainMenu(side);
  side.ev("friends_host_ready");
  await peerDone;
}

async function friendsJoin(side, host, room, results) {
  await side.expect(/^orca caps /, 180000);
  side.send(CAPS);
  await toMainMenu(side);
  for (let n = 1; n <= MATCHES; ++n) {
    const r = { n, side: side.name, ok: false, role: "join", room };
    results.push(r);
    try {
      // The host has been on its own long enough that the friend's arrival fresh-starts it.
      await sleep(FRIEND_IDLE_MS + (n > 1 ? GAP_MS : 0));
      if (host) host.send("prepare-join");
      await sleep(300);
      const errFrom = side.err.length;
      const hostOut = host ? host.lines.length : 0;
      side.ev("matched", { n, room, slot: 1, friends: true });
      if (host) host.ev("matched", { n, room, slot: 0, friends: true });
      side.startShots(`m${n}`);
      if (host) host.startShots(`m${n}`);
      side.send(`join ${room}`);
      await side.expectErr(
        /Drop-in: plugged into port \d at frame/,
        60000,
        errFrom,
      );
      if (host) await host.expect(/^orca state friend-joined$/, 10000, hostOut);
      side.ev("endpoint", { n });
      if (host) host.ev("endpoint", { n });
      r.ok = true;
      await sleep(HOLD_MS);
      side.stopShots();
      if (host) host.stopShots();
      const backFrom = side.err.length;
      side.send("leave");
      await side.expectErr(
        /Drop-in: back to solo play|Drop-in: home /,
        60000,
        backFrom,
      );
      if (host) await host.expect(/^orca state friend-left/, 20000, hostOut);
      await sleep(3000);
    } catch (e) {
      r.error = String(e.message || e);
      side.ev("match_error", { n, error: r.error });
      log(`${side.name}: friends join ${n} failed: ${r.error}`);
      side.stopShots();
      side.send("leave");
      await sleep(5000);
    }
  }
}

async function main() {
  const meta = {
    args: argv,
    mode: MODE,
    slug: SLUG,
    site: SITE,
    platform: process.platform,
    host: os.hostname(),
    start: Date.now(),
    orca_build: ORCA_BUILD,
    env: Object.fromEntries(
      Object.entries(process.env).filter(
        ([k]) => k.startsWith("ORCA_TEST_") || k.startsWith("SDL_"),
      ),
    ),
  };
  meta.clock_before = await edgeOffset();
  log(
    `work ${WORK}; out ${OUT}; mode ${MODE}; edge clock offset ${JSON.stringify(meta.clock_before)}`,
  );
  const sides = [];
  const keys = PAIR ? ["a", "b"] : [ONE];
  for (const k of keys) sides.push(new Side(k, k === "a" ? NAME_A : NAME_B));
  const results = [];
  let failed = null;
  try {
    for (const s of sides) await s.startBridge();
    if (MODE === "friends") {
      const room = FRIEND_ROOM || `ml${crypto.randomBytes(4).toString("hex")}`;
      meta.friend_room = room;
      for (const s of sides) {
        s.friendHost = s.key === "a";
        s.friendRoom = room;
      }
    }
    // The second Orca boots a little later, as two players do.
    for (const [i, s] of sides.entries()) {
      s.start(s.key === "b" ? BIN_B : BIN);
      if (i === 0 && sides.length > 1) await sleep(3000);
    }
    if (MODE === "friends") {
      const host = sides.find((s) => s.key === "a");
      const join = sides.find((s) => s.key === "b");
      let done;
      const peerDone = new Promise((r) => (done = r));
      const jobs = [];
      if (host)
        jobs.push(
          friendsHost(
            host,
            join
              ? peerDone
              : sleep(MATCHES * (FRIEND_IDLE_MS + HOLD_MS + 20000)),
          ),
        );
      if (join)
        jobs.push(
          friendsJoin(join, host, meta.friend_room, results).finally(done),
        );
      await Promise.all(jobs);
    } else {
      await Promise.all(sides.map((s) => queueLoop(s, QUEUE, results)));
    }
  } catch (e) {
    failed = String(e.message || e);
    log(`stopped: ${failed}`);
  } finally {
    for (const s of sides) s.stop();
    await sleep(3000);
    for (const s of sides) {
      try {
        s.collect();
      } catch (e) {
        log(`collect ${s.name}: ${e.message}`);
      }
      s.raw.end();
    }
  }
  meta.clock_after = await edgeOffset();
  meta.end = Date.now();
  meta.failed = failed;
  meta.results = results;
  fs.writeFileSync(path.join(OUT, "meta.json"), JSON.stringify(meta, null, 2));
  const ok = results.filter((r) => r.ok).length;
  console.log(
    `MLAT ${JSON.stringify({ out: OUT, mode: MODE, ok, total: results.length, failed })}`,
  );
  try {
    fs.rmSync(WORK, { recursive: true, force: true });
  } catch {}
  process.exit(failed ? 1 : 0);
}

main();
