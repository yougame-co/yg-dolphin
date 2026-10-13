#!/usr/bin/env node
// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
//
// queue-e2e.mjs: two headless Orcas matched by YouGame's real queue (ORCA.md "Matchmaking"). Each
// sits behind an in-process stand-in for the desktop app's bridge that plays the YouGame page's
// part (web/src/lib/orca-queue.ts, components/OrcaEmbed.tsx, Player.tsx): it answers `orca caps`,
// mints room and queue tickets, opens the rooms Worker's /match, sends `host`/`join` once matched,
// and serves the controller stream whose pad this script presses (menus, character select, stage,
// and port 2 walking off the stage until it runs out of stocks). The test checks the stdout
// sequence and that both Orcas print the same verdicts.
//
// --q2 runs the queue2 flow (ORCA.md "The queue's character select"): each side picks on its own
// character select and `orca queue ready` starts the search; once matched, the joiner's pick is put
// into the host's game. Scenarios with --q2 (--scenario):
//   match    the default: a casual game or a ranked set to its end (VICTORY/DEFEAT, new rating),
//            then both leave the room through the results screen
//   skip     the joiner holds B (`orca queue skip`); both end up back on their own character select
//   rematch  casual: skip, then both search again and meet in a new room, the skipper hosting
//   timeout  casual: nobody readies; both time out and are back on their own character select
//   kill     ranked: the joiner's Orca is killed in game 2; the host wins by forfeit after 15 s
//   leave    ranked: as kill, but the joiner sends `leave`
//            --in-game 1: kill or leave during game 1 instead. Both Orcas must first have sent the
//            room one `game-start` with the same id (Orca 0.3.33+, once the fighters are in), so the
//            leave still loses the set by forfeit; --expect void (with --in-game 1 only) is the
//            control for an Orca without it: no game-start, and the set ends void ("ended before a
//            game was played")
//            --in-game 0: kill or leave 1 s into game 1's stage, during its loading image, before
//            the fighters are in: neither Orca sends game-start, and the set ends void, nobody
//            rated (a crash at load; --expect void is then the default)
//   bleave   ranked: after the set the joiner leaves by holding B on the results screen (zleave, its
//            old name, still runs it)
//   rejoin   ranked: the joiner's room connection drops in game 1 (`test-drop-room`) and its page
//            sends it back to the same room, as the YouGame page does; it must plug in again and
//            play on (bug reports 6afb0948, 22ba6446, 96e56887: Orca 0.3.30 and 0.3.34 ended with
//            "The host plugged this controller in at frame N, already past"). --drop silent (the
//            default) drops it as a network failure does (ORCA_TEST_DROP_SILENT: the server hears
//            of it only when the next connection replaces it); --drop clean closes it at once
//   noskip   ranked: both hold B on every character select; ranked has no skip
//   altz     Project+: both hold Z through the first character select's go (Orca's Start), as the
//            AltZ costume code wants (hidden costume 62); Z must reach both games alike, which the
//            checksum gate checks. The gate can't tell whether Z reached the game at all: add
//            --probe Tools/orca/inputs/pplus-altz.probe and each Orca's log shows
//            `Watch ... p1-9C = 003e....` and `p2-9C = 003e....` (costume 62) as the select ends
// --pick random makes every pick RANDOM. Every --q2 scenario ends with the checksum gate: each
// session either Orca ended matched checksums with the other, and neither logged a desync.
//
//   node Tools/orca/queue-e2e.mjs <dolphin-emu-nogui> <disc> [--pplus <launcher.dol>]
//       [--queue ranked|casual] [--games <n>] [--site <url>] [--keep] [--alarm <s>]
//       [--probe <file>] [--controls <ada>,<bo>] [--controls-mid <bo>]
//       [--slug <listing> --cookie-a <file> --cookie-b <file> --id-a <id> --id-b <id>]
//       [--disc-b <disc>] [--delay <n>]
//
// --disc-b: the second player (bo) boots this disc (Brawl Rev 1 against Rev 2, or the reverse).
// Without --slug: dev tickets (an anonymous dev game of this run's own, nothing recorded, keyframes
// in a local folder).
// With --slug: real tickets for that listing as two signed-in players (the cookie files hold a
// session's Cookie header, mode 600, never printed), keyframes through YouGame's store.
// Orca runs nice'd, muted, with Null video and a perl alarm. On a shared machine, don't run it
// while someone is playing or the load is high.
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
const DISC_B = opt("--disc-b", DISC);
const QUEUE = opt("--queue", "ranked");
const GAMES = Number(opt("--games", QUEUE === "ranked" ? "3" : "2"));
const SITE = (opt("--site", "https://yougame.co") || "").replace(/\/$/, "");
const SLUG = opt("--slug");
const KEEP = argv.includes("--keep");
// YG_SITE_COOKIE: a Cookie header value ("name=value") sent with every request to --site, such as a
// staging site's team access cookie, handed in by a wrapper. Never logged.
const SITE_COOKIE = process.env.YG_SITE_COOKIE || "";
const siteCookie = (own) => [own, SITE_COOKIE].filter(Boolean).join("; ");
const ALARM = Number(opt("--alarm", "900"));
// Scenarios without --q2:
//   match    the default: a ranked set, or --games casual games and then the joiner leaves
//   cancel   one Orca picks, then backs out of the character select while searching
//   forfeit  ranked: the joiner's connection drops after game 1; the host wins by forfeit
//   rehost   casual: after the joiner leaves, it queues first, hosts, and they play again
//   noshow   the opponent never arrives; the host moves on to a room of its own
// zleave is bleave's old name (leaving moved from Z to B).
const SCENARIO = opt("--scenario", "match") === "zleave" ? "bleave" : opt("--scenario", "match");
if (SCENARIO === "altz" && !argv.includes("--pplus")) {
  console.error("--scenario altz is for Project+ (--pplus)");
  process.exit(2);
}
// --shots <n>: render with Metal and save a screenshot every n frames from --shots-from; 0 saves
// only the scenario's own shots (`test-shot`). --shots-who picks which Orcas render (ada, bo or
// ada,bo), --shots-size sets the size, and --shots-out copies the scenario's shots to a folder.
const SHOTS = opt("--shots");
const SHOTS_FROM = opt("--shots-from", "1500");
const SHOTS_WHO = (opt("--shots-who", "ada") || "").split(",");
const SHOTS_SIZE = opt("--shots-size", "640x528");
const SHOTS_OUT = opt("--shots-out");
// --music-off ada|bo|ada,bo: that player's Music switch is off (`music off` after the caps answer,
// ORCA.md "Music switch"). --dump-audio: each Orca writes what it plays to
// user-<name>/Dump/Audio/dspdump.wav (keep it with --keep).
const MUSIC_OFF = (opt("--music-off", "") || "").split(",").filter(Boolean);
const DUMP_AUDIO = argv.includes("--dump-audio");
// --replay: each Orca writes its history as replay streams to <work>/rec-<name>-<n>.ygr
// (ORCA_TEST_REPLAY_OUT; Tools/orca/replay-set-check.py watches and replays them).
const REPLAY = argv.includes("--replay");
// How long after a ranked set's verdict the page sends the new rating (`queue rating`).
const RATING_AFTER_MS = Number(opt("--rating-after", "1500"));
const Q2 = argv.includes("--q2");
// --delay <n>: each page sends `delay <n>` after its caps, as the YouGame page does (prod: 3).
// Without it Orca plays its own default (0.3.33+: a fixed 3; 0.3.32 and older: adaptive from 2), so
// a perf gate across those builds passes --delay to both.
const DELAY = opt("--delay");
if (DELAY !== undefined && !/^[1-9]$/.test(DELAY)) {
  console.error("--delay is a number of frames, 1 to 9");
  process.exit(2);
}
// --scenario playagain (ranked, --q2): "Play again" after a ranked set. After the verdict, the
// inviter's page invites the other player into the private room its Orca hosts (`prepare-join`) and
// the invitee's page accepts it (`join <code>`, --invite-gap ms later). --variant picks where each
// player is then:
//   a: the invitee is still in the set's room as its host (the joiner, the inviter, left by holding
//      B on the results screen; the host stays there);
//   b: the invitee is still in the set's room as its joiner (the host, the inviter, held B);
//   c: both are still in the set's room when the invite goes out: the inviter's page leaves the room
//      (the app's `leave`) and invites at once, and the invitee accepts as soon as the code is known;
//   d: both left together by pressing on through the results screen, each in a room of its own
//      (--inviter joiner|host, default joiner);
//   e: as d, but the inviter backs out of its queue's character select to the main menu first (the
//      control: no queue header in the inviter's game).
// The join either plugs in (the inviter prints `orca state friend-joined`, the invitee `orca state
// playing`) or fails; the run prints a RESULT line and exits 0 (joined) or 3 (the join failed).
// Once joined, the inviter leaves (`leave`) and the invitee must go home to a room of its own
// (ORCA.md "Drop-in" ComeHome), never `orca state no-room` (q14: a joiner from its own queue select
// kept that game for after a queue room, which stopped it going home); --no-after-leave skips it.
// RESULT then says HOME or NO-ROOM; exit 4 for the latter.
const AFTER_LEAVE = !argv.includes("--no-after-leave");
// Once joined: --together <ms> (default 20000) together in the new room before the inviter leaves,
// and with --fight they play a game there in that time (both pick on the inviter's character select,
// its Start, a stage, port 2 walks off until the results, both press on back to the select). After
// the leave, each side's session from the join on must have matched checksums (its "back to solo
// play" line counts more than 0) over at least 15 s of frames (900 from the invitee's plug-in), with
// no desync in either log: RESULT: TOGETHER ..., else TOGETHER-FAILED and exit 5.
const TOGETHER_MS = Number(opt("--together", "20000"));
const FIGHT = argv.includes("--fight");
// --leaver invitee: after that time the friend (the invitee) leaves instead of the inviter, and the
// inviter, alone again on the select its queue's pick opened, must have that pick armed again
// (`orca menu online <queue>`, RearmFriendsPick) within 20 s while the friend goes home to a room of
// its own: RESULT: REARMED / NOT-REARMED (exit 6) and HOME / NO-ROOM.
const LEAVER = opt("--leaver", "inviter");
// --scenario kill|leave: the game of the set the joiner goes in (1 or 2, or 0: game 1's load, before
// its fighters are in), and the set's expected end (forfeit, or void for an Orca that never says a
// game began, and for a leave at load).
const IN_GAME = Number(opt("--in-game", "2"));
const EXPECT = opt("--expect", IN_GAME === 0 ? "void" : "forfeit");
if (![0, 1, 2].includes(IN_GAME) || !["forfeit", "void"].includes(EXPECT)) {
  console.error("--in-game is 0, 1 or 2; --expect is forfeit or void");
  process.exit(2);
}
// A set that played game 1 reached it whatever the Orca: only a leave in game 1 can be void, and a
// leave at game 1's load always is.
if ((EXPECT === "void" && IN_GAME === 2) || (EXPECT === "forfeit" && IN_GAME === 0)) {
  console.error("--expect void needs --in-game 0 or 1; --in-game 0 is void");
  process.exit(2);
}
// --join-delay <ms>: how long after the match the joiner's page sends its first `join` (default 1500).
// The YouGame page sends `host` and `join` at once (lib/orca-queue.ts ORCA_JOIN_DELAY_MS 0); with 0
// the host's fresh start, its hold for the room's header and its own pick being put back all race
// the joiner's arrival, as in prod.
const JOIN_DELAY_MS = Number(opt("--join-delay", "1500"));
const DROP = opt("--drop", "silent");
// --costume <n> (--q2): after A on its own character select each player presses X n times, so its
// pick is in another colour and the host's own pick, put back after its fresh start, needs X too.
const COSTUME_PRESSES = Number(opt("--costume", "0"));
const VARIANT = opt("--variant", "d");
const INVITER = opt("--inviter", "");
const INVITE_GAP_MS = Number(opt("--invite-gap", "4000"));
// The page's answer to `orca caps` (YouGame lib/orca.ts orcaCapsAnswer): what Orca offers of these.
const PAGE_CAPS = new Set([
  "join", "leave", "stats", "pause", "delay", "perf", "direct", "host", "results", "locks", "queue2",
  "pick-casual", "pick-ranked",
]);
// --pick random (--q2): both pick the RANDOM tile (ORCA.md "The queue's character select"); the
// verdicts' resolved characters must match on both sides.
const PICK = opt("--pick", "character");
// --probe <file> (dev tickets only): both Orcas run that ORCA_UX_PROBE file (Orca/UX/Probe.cpp, a
// research knob), so its writes land in each player's own game before any session; for example a
// Project+ record with CPUs written at the main menu (ORCA.md "No CPUs online").
const PROBE = opt("--probe");
// --controls <ada>,<bo>: each player's own controls at launch (ORCA_CONTROLS: the 92-hex profile of
// UX/NameTags.h, "-" for none), as the app passes the saved ones. Each fight then starts with both
// pressing Z three times in turn, so a probe's `fighters` line shows what Z does on both screens.
const CONTROLS = (opt("--controls", "") || "").split(",");
// --controls-mid <hex> (--q2): bo's own controls from a `controls` line once Orca is up, as the page
// sends a change made in its Controls panel.
const CONTROLS_MID = opt("--controls-mid");
if (PROBE && SLUG) {
  console.error("--probe is for dev tickets only");
  process.exit(2);
}
// Random's character id (Selch_Random), and how many frames of full tilt up, then right, take the
// hand from panel 1 onto the RANDOM tile in each game.
const RANDOM_CHAR = 41;
const RANDOM_PATH = PPLUS ? { up: 20, right: 24 } : { up: 22, right: 58 };
// --pick bowser (Project+, --q2): both pick Bowser (up, then right from panel 1).
const BOWSER_CHAR = 12;
const BOWSER_PATH = { up: 33, right: 28 };
if (PICK === "bowser" && !PPLUS) {
  console.error("--pick bowser is for Project+");
  process.exit(2);
}
// Project+'s Code Menu under a ranked or casual header (ORCA.md "Project+'s Code Menu and banned
// fighters"). --giga: both hold L through the first character select's Start, which makes a Bowser
// pick Giga Bowser unless Orca turns it back. --chord: both press L+R+D-pad Down every 2.5 s of
// each fight and once on each results screen, which opens the menu unless Orca keeps it shut.
const GIGA = argv.includes("--giga");
const CHORD = argv.includes("--chord");
const PROFILE = PPLUS ? "pplus32" : "rsbe01";
// Each run gets a dev game of its own, so two runs at once never meet in the queue (correctness runs
// share the Mac: Tools/orca/run-lock.py). QE2E_DEV_SUFFIX picks the suffix; empty means the old
// shared game, orca-queue-e2e-<profile>, which two runs at once must not use.
const DEV_SUFFIX = (
  process.env.QE2E_DEV_SUFFIX ?? crypto.randomBytes(4).toString("hex")
)
  .toLowerCase()
  .replace(/[^a-z0-9]+/g, "-")
  .replace(/^-+|-+$/g, "");
const DEV_GAME = `orca-queue-e2e-${PROFILE}${DEV_SUFFIX ? `-${DEV_SUFFIX}` : ""}`
  .slice(0, 60)
  .replace(/-+$/, "");
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
  R: 1024,
  L: 2048,
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
    this.queueLog = []; // Orca's "Queue: ..." log lines (UX/Queue.h, OnlineMatch's queue image)
    this.sessions = []; // each session's end: { frame, why, rollbacks, checksums }
    this.desyncs = []; // "Desync at frame ..." and "the session failed" log lines
    this.setEndLog = []; // { at, line }: "Set end: ..." (UX/SetEnd.h) and the host's stall break
    this.rating = -1; // the rating the page gave (`queue rating`)
    this.shots = []; // the scenario's own screenshots (test-shot)
    this.joinLog = []; // { at, line }: drop-in and room lines (--scenario playagain)
    this.gameStarts = []; // { at, msg }: the room's game-start messages this Orca sent
  }

  // The scenario's own screenshot (`test-shot`), when this Orca renders.
  shot(name) {
    if (!SHOTS || !SHOTS_WHO.includes(this.name)) return;
    this.send(`test-shot ${name}`);
    this.shots.push(name);
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
              ? [this.pad.buttons & BTN.L ? 255 : 0, this.pad.buttons & BTN.R ? 255 : 0]
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
    // As Player.tsx: note the mode and lobby of Orca's own room; the matched room gets the queue's
    // ticket, signed for that room only.
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
        ...(siteCookie(this.cookie) ? { cookie: siteCookie(this.cookie) } : {}),
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
    if (REPLAY) {
      env.ORCA_TEST_REPLAY_OUT = path.join(WORK, `rec-${this.name}`);
      env.ORCA_TEST_DEV_GAME = DEV_GAME; // test knobs work in dev games only
    }
    // Test overrides from this script's own environment, the same for both Orcas (for example
    // ORCA_TEST_SET_RULES=1,8: a ranked Brawl set's games at 1 stock), and SDL's switches (on a
    // shared Mac, SDL_JOYSTICK_HIDAPI=0 and friends keep test Orcas off a player's adapter).
    for (const [k, v] of Object.entries(process.env)) {
      if ((k.startsWith("ORCA_TEST_") || k.startsWith("SDL_")) && !(k in env)) env[k] = v;
    }
    if (PPLUS) env.ORCA_PROFILE = "PPLUS32";
    if (SCENARIO === "rejoin" && DROP === "silent") env.ORCA_TEST_DROP_SILENT = "1";
    const own = CONTROLS[this.name === "ada" ? 0 : 1];
    if (own && own !== "-") env.ORCA_CONTROLS = own;
    if (PROBE) {
      env.ORCA_UX_PROBE = path.resolve(PROBE);
      env.ORCA_TEST_DEV_GAME = DEV_GAME;
    }
    const shots = SHOTS && SHOTS_WHO.includes(this.name);
    if (shots)
      Object.assign(env, {
        YG_SHOT_EVERY: SHOTS,
        YG_SHOT_FROM: SHOTS_FROM,
        ORCA_TEST_PRESENT: SHOTS_SIZE,
      });
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
      shots ? "Metal" : "Null",
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
      ...(DUMP_AUDIO
        ? ["-C", "Dolphin.DSP.DumpAudio=True", "-C", "Dolphin.DSP.DumpAudioSilent=True"]
        : []),
      // --shots 0: keep Dolphin's "Screenshot saved" OSD messages out of the scenario's shots.
      ...(shots && SHOTS === "0" ? ["-C", "Dolphin.Interface.OnScreenDisplayMessages=False"] : []),
      ...(PPLUS
        ? ["-C", `Dolphin.Core.DefaultISO=${this.name === "bo" ? DISC_B : DISC}`, "-e", PPLUS]
        : ["-e", this.name === "bo" ? DISC_B : DISC]),
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
        const q = /(Queue: .*)$/.exec(line);
        if (q) {
          this.queueLog.push(q[1]);
          log(`${this.name}~ ${q[1]}`);
        }
        const solo =
          /Drop-in: back to solo play at frame (\d+) \(([^)]*)\): (\d+) rollbacks, (\d+) checksums matched/.exec(
            line,
          );
        if (solo) {
          this.sessions.push({
            frame: Number(solo[1]),
            why: solo[2],
            rollbacks: Number(solo[3]),
            checksums: Number(solo[4]),
          });
          log(`${this.name}~ ${solo[0]}`);
        }
        const se =
          /(Set end: .*|Drop-in: port \d left the room while .*|Matchmaking: set over: .*|Orca room: set over.*)$/.exec(
            line,
          );
        if (se) {
          this.setEndLog.push({ at: Date.now(), line: se[1] });
          log(`${this.name}~ ${se[1]}`);
        }
        const pj =
          /(Online rules: (the host's game says|the host's header|leaving room|header ).*|Drop-in: (left|loaded keyframe|the host offers|joining room|plugged into|playing solo|leaving|this boot's NAND|port \d (left|moves|is)|keyframe of frame|.* arrived for port|back to a game|home |fresh start at frame ).*|Couldn't .*|Orca room ended: .*|Orca: joining a YouGame room.*|Orca room: in room .*|Orca room: Room \S+: .*|Matchmaking: (the opponent|the host|done|no verdict|the room closed).*|Online: session ended.*)$/.exec(
            line,
          );
        if (pj) {
          this.joinLog.push({ at: Date.now(), line: pj[1] });
          if (SCENARIO === "playagain") log(`${this.name}~ ${pj[1]}`);
        }
        const gs = /Orca room: (\{"t":"game-start".*\})$/.exec(line);
        if (gs) this.gameStarts.push({ at: Date.now(), msg: JSON.parse(gs[1]) });
        if (/Desync at frame|the session failed/.test(line)) {
          this.desyncs.push(line);
          log(`${this.name}~ ${line}`);
        }
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
      if (line.startsWith("orca result ")) {
        const r = JSON.parse(line.slice(12));
        this.results.push(r);
        // The page's second `queue rating`: the new rating, once the site rated the set.
        if (Q2 && r.q === "ranked" && r.k === "set" && r.out !== "void" && this.rating >= 0) {
          const after = this.rating + (r.out === "won" ? 16 : -15);
          setTimeout(() => {
            if (this.exited === undefined) this.send(`queue rating ${after}`);
          }, RATING_AFTER_MS);
          this.rating = after;
        }
      }
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
    // --music-off: this page turns `music` on in its caps answer and sends the switch after it.
    const music = line.startsWith("caps") && MUSIC_OFF.includes(this.name);
    if (music && !this.lines.some((l) => l && /^orca caps .*\bmusic\b/.test(l)))
      fail(`${this.name}: Orca offers no music`);
    const outs = music ? [`${line} music`, "music off"] : [line];
    // --delay: the page's input delay right after its caps answer, as OrcaEmbed sends it.
    if (DELAY && line.startsWith("caps") && /\bdelay\b/.test(line)) outs.push(`delay ${DELAY}`);
    for (const out of outs) {
      log(`${this.name}< ${out}`);
      this.proc.stdin.write(`${out}\n`);
    }
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

  async pressAll(buttons, ms = 120) {
    for (const b of buttons) this.pad.buttons |= BTN[b];
    await sleep(ms);
    for (const b of buttons) this.pad.buttons &= ~BTN[b];
    await sleep(120);
  }

  async press(button, ms = 120) {
    this.pad.buttons |= BTN[button];
    await sleep(ms);
    this.pad.buttons &= ~BTN[button];
    await sleep(120);
  }

  // L+R held, D-pad Down pressed: Project+'s Code Menu chord.
  async chord() {
    this.pad.buttons |= BTN.L | BTN.R;
    await sleep(150);
    this.pad.buttons |= BTN.DOWN;
    await sleep(100);
    this.pad.buttons &= ~BTN.DOWN;
    await sleep(150);
    this.pad.buttons &= ~(BTN.L | BTN.R);
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
  // Mash past the strap, boot and title screens, but stop once the title leaves: a stray A on the
  // main menu would pick Group (VERSUS in Project+).
  const mash = setInterval(() => {
    if (["", "scStrap", "scBoot", "scTitle"].includes(side.scene)) side.press("A");
  }, 700);
  await side.waitScene("muMenuMain", 120000);
  clearInterval(mash);
  await sleep(2500);
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
async function queueUp(side, queue, from = 0) {
  const menu = await side.expect(
    /^orca menu (online (casual|ranked|friends)( local)?|cancel)$/,
    60000,
    from,
  );
  if (menu.m[3])
    fail(`${side.name}: "${menu.line}": Orca says local with host on`);
  if (menu.m[2] !== queue)
    fail(`${side.name}: picked ${menu.m[2]}, wanted ${queue}`);
  return search(side, queue);
}

// The page's search: a queue ticket in Orca's own mode and lobby, then /match until matched.
async function search(side, queue) {
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
  side.queueWs = ws;
  side.waiting = 0;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`${side.name}: no match in 120 s`)),
      120000,
    );
    ws.onclose = (e) => {
      clearTimeout(timer);
      if (side.queueWs === ws) side.queueWs = null;
      reject(new Error(`${side.name}: /match closed (${e.code} ${e.reason})`));
    };
    ws.onmessage = async (e) => {
      const m = JSON.parse(e.data);
      if (m.t === "searching") {
        side.waiting = m.waiting;
        log(`${side.name}: searching, ${m.waiting} waiting`);
      }
      if (m.t !== "matched") return;
      clearTimeout(timer);
      ws.onclose = null;
      side.queueWs = null;
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
    await sleep(tries === 0 ? JOIN_DELAY_MS : 1500);
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

// One game, driven from the host's scenes (the host never loads a keyframe, so its scene lines are
// exact): both pick, the host starts and picks the stage, port 2 walks off until the fight ends,
// and both press on through the results.
async function playGame(host, joiner, first) {
  await host.waitScene("scSelctCharacter", 120000);
  await sleep(2500);
  if (first) {
    await Promise.all([host.press("A"), joiner.press("A")]);
    await sleep(600);
    await Promise.all([
      // Up from the panel into the middle of the grid: further reaches the top bar and BACK.
      host.hold([128, 255, 128, 128], 450),
      joiner.hold([128, 255, 128, 128], 450),
    ]);
    await sleep(300);
    await Promise.all([host.press("A"), joiner.press("A")]);
    await sleep(1500);
  }
  // A ranked set's later games have a character order (CharOrder.h). The hands start on the bottom
  // panels, where A is masked (it would make the player a CPU), so move up into the grid first.
  const cssTries = QUEUE === "ranked" ? 120 : 25;
  if (QUEUE === "ranked" && !first) {
    await Promise.all([
      host.hold([128, 255, 128, 128], 450),
      joiner.hold([128, 255, 128, 128], 450),
    ]);
    await sleep(300);
  }
  for (let tries = 0; host.scene === "scSelctCharacter"; ++tries) {
    if (tries >= cssTries) fail("the character select never started the match");
    if (QUEUE === "ranked" && !first && tries < 3) {
      await Promise.all([host.press("A"), joiner.press("A")]);
      await sleep(400);
    }
    await host.press("START");
    await sleep(1200);
  }
  if (QUEUE === "casual") {
    // Casual stage pick: nobody acts, so each player gets a random legal stage and a coin flip
    // takes one.
    await host.waitScene("scMelee", 60000);
  } else {
    // Ranked strikes, ban and pick: nobody acts, so every turn timer runs out and takes its default.
    await host.waitScene("scSelStage", 60000);
    log("ranked stage select: waiting for the turn timers' defaults");
    await host.waitScene("scMelee", 200000);
  }
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

// Cancel a search twice, once from the page and once by backing out of the character select, then
// check that a second searcher finds nobody: the cancelled tickets must be gone from the queue.
async function cancelScenario() {
  const a = new Side("ada", opt("--cookie-a"), opt("--id-a"));
  const b = new Side("bo", opt("--cookie-b"), opt("--id-b"));
  sides.push(a);
  await a.startBridge();
  a.start();
  await a.expect(/^orca caps .*\bhost\b/, 120000);
  a.send("caps join leave stats pause delay perf direct host results");
  await menuToQueue(a, QUEUE);
  // 1. Searching, then the page's Cancel.
  let search = queueUp(a, QUEUE).catch((e) => e);
  await waitFor(() => a.waiting >= 1, 30000, "ada: never searching");
  await a.waitScene("scSelctCharacter", 30000);
  await sleep(2000);
  a.queueWs.close(1000, "cancelled");
  a.send("queue-cancel");
  await search;
  await sleep(1500);
  // Back on the menu after the page's Cancel: the search was already over, so no `orca menu cancel`.
  let from = a.lines.length;
  await a.press("B", 1500);
  await a.waitScene("muMenuMain", 30000);
  await sleep(4000);
  if (a.lines.slice(from).some((l) => l === "orca menu cancel"))
    fail("orca menu cancel after the page had cancelled the search");
  log("PASS: the page's Cancel ended the search (queue-cancel); backing out after it printed nothing");
  // 2. The same pick again, searching, then the player backs out of the character select.
  from = a.lines.length;
  await a.press("A");
  search = queueUp(a, QUEUE, from).catch((e) => e);
  await waitFor(() => a.waiting >= 1, 30000, "ada: never searching the second time");
  await a.waitScene("scSelctCharacter", 30000);
  await sleep(3000);
  from = a.lines.length;
  await a.press("B", 1500);
  await a.expect(/^orca menu cancel$/, 30000, from);
  a.queueWs.close(1000, "cancelled");
  a.send("queue-cancel");
  await search;
  log("PASS: backing out of the character select while searching printed orca menu cancel");
  // 3. Nobody is left in the queue: a second searcher in the same queue waits alone.
  const mode = `${a.lastOwn.mode}-${QUEUE}`;
  const tk = await b.ticket({
    players: 2,
    mode,
    lobby: a.lastOwn.lobby,
    queue: QUEUE,
    ...(QUEUE === "casual" ? { after_match: "stay" } : {}),
  });
  const seen = await new Promise((resolve) => {
    const got = [];
    const ws = new WebSocket(
      `${tk.url.replace(/^http/, "ws")}/match?ticket=${encodeURIComponent(tk.ticket)}`,
    );
    ws.onmessage = (e) => got.push(JSON.parse(e.data));
    setTimeout(() => {
      ws.close(1000, "cancelled");
      resolve(got);
    }, 8000);
  });
  if (seen.some((m) => m.t === "matched"))
    fail(`a cancelled ticket was still in the queue: ${JSON.stringify(seen)}`);
  const waiting = seen.filter((m) => m.t === "searching").map((m) => m.waiting);
  if (!waiting.length || Math.max(...waiting) !== 1)
    fail(`the second searcher saw ${JSON.stringify(waiting)} searching, wanted 1`);
  log("PASS: after both cancels, a second searcher waits alone (1 searching), never matched");
}

// The opponent's app closes the moment they are matched. After ORCA_HOST_WAIT_MS the host's page
// sends `leave` and Orca moves to a fresh room of its own. No game began, so nothing is recorded.
async function noShowScenario() {
  const a = new Side("ada", opt("--cookie-a"), opt("--id-a"));
  const b = new Side("bo", opt("--cookie-b"), opt("--id-b"));
  sides.push(a, b);
  for (const s of [a, b]) await s.startBridge();
  a.start();
  await sleep(4000);
  b.start();
  for (const s of [a, b]) {
    await s.expect(/^orca caps .*\bhost\b/, 120000);
    s.send("caps join leave stats pause delay perf direct host results");
  }
  const from = [a.lines.length, b.lines.length];
  await menuToQueue(a, QUEUE);
  const qa = queueUp(a, QUEUE, from[0]);
  await sleep(6000);
  await menuToQueue(b, QUEUE);
  const qb = queueUp(b, QUEUE, from[1]);
  const [ma, mb] = await Promise.all([qa, qb]);
  if (ma.room !== mb.room) fail(`matched into different rooms: ${ma.room} ${mb.room}`);
  const host = ma.slot === 0 ? a : b;
  const gone = host === a ? b : a;
  // The opponent's app closes: its Orca is gone before its page would have sent `join`.
  gone.proc.kill("SIGKILL");
  log(`${gone.name}'s app closed at the match; ${host.name} hosts ${ma.room} alone`);
  const hostFrom = host.lines.length;
  const roomsBefore = host.ownRooms.length;
  host.send(`host ${ma.room}`);
  // The page's no-show watch: nobody else in the room after 30 s.
  await sleep(30000);
  const me = host.mirror?.me ?? host.playerId;
  const others = (host.mirror?.participants || []).filter((p) => p.id !== me);
  if (host.mirror?.code && host.mirror.code !== ma.room)
    log(`the host's mirror is room ${host.mirror.code}`);
  if (others.length) fail(`someone is in the room: ${JSON.stringify(others)}`);
  if (host.lines.slice(hostFrom).some((l) => l === "unsupported host")) fail("unsupported host");
  host.send("leave");
  await host.expect(/^orca state left$/, 30000, hostFrom);
  await waitFor(
    () => host.ownRooms.length > roomsBefore && host.ownRooms.at(-1) !== ma.room,
    30000,
    "the host opened no room of its own after leaving",
  );
  noErrors([host]);
  log(`PASS: no-show: the host left ${ma.room} after 30 s and opened ${host.ownRooms.at(-1)}`);
  console.log(`NOSHOW_ROOM ${ma.room}`);
}

async function waitFor(cond, ms, why) {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) fail(why);
    await sleep(250);
  }
}

const sides = [];

// Both sides pick in the menu (`first` picks first, so it waits longest and gets slot 0, the host),
// queue, and meet in the matched room: the host's `friend-joined`, the joiner's `playing`.
async function meet(first, second, queue, pick) {
  const from = [first.lines.length, second.lines.length];
  const queued = [];
  await pick(first);
  queued.push(queueUp(first, queue, from[0]));
  await sleep(6000);
  await pick(second);
  queued.push(queueUp(second, queue, from[1]));
  const [m1, m2] = await Promise.all(queued);
  if (m1.room !== m2.room) fail(`matched into different rooms: ${m1.room} ${m2.room}`);
  const host = m1.slot === 0 ? first : second;
  const joiner = host === first ? second : first;
  log(`host ${host.name}, joiner ${joiner.name}, room ${m1.room}`);
  const hostFrom = host.lines.length;
  await Promise.all([hostOrJoin(first, m1.room, m1.slot), hostOrJoin(second, m2.room, m2.slot)]);
  await host.expect(/^orca state friend-joined$/, 120000, hostFrom);
  if (host.lines.slice(hostFrom).some((l) => l === "unsupported host")) fail("unsupported host");
  return { host, joiner, room: m1.room };
}

// Both printed the same verdicts, each from its own side, and no errors.
function sameVerdicts(host, joiner, fromHost = 0, fromJoiner = 0) {
  const ga = host.results.slice(fromHost),
    gb = joiner.results.slice(fromJoiner);
  if (ga.length !== gb.length)
    fail(`${ga.length} result lines on the host, ${gb.length} on the joiner`);
  for (let i = 0; i < ga.length; ++i)
    if (!mirrored(ga[i], gb[i]))
      fail(`result ${i + 1} differs: ${JSON.stringify(ga[i])} / ${JSON.stringify(gb[i])}`);
  return ga.length;
}

// The checksum gate: each Orca ended at least `want` sessions, every one matched checksums with the
// other side, and neither log has a desync or failed session. Returns a summary for the PASS line.
function checksumsMatched(sides, want = 1) {
  for (const s of sides) {
    if (s.desyncs.length) fail(`${s.name}: ${s.desyncs[0]}`);
    if (s.sessions.length < want)
      fail(`${s.name}: ${s.sessions.length} session(s) ended, wanted ${want}`);
    const none = s.sessions.find((x) => x.checksums <= 0);
    if (none)
      fail(`${s.name}: the session that ended at frame ${none.frame} (${none.why}) matched no checksums`);
  }
  return sides
    .map(
      (s) =>
        `${s.name} ${s.sessions.map((x) => `${x.checksums} (${x.why}, ${x.rollbacks} rollbacks)`).join(" + ")}`,
    )
    .join("; ");
}

function noErrors(sides, allowed = /^$/) {
  for (const s of sides) {
    const errors = s.lines.filter(
      (l) => l && l.startsWith("orca error ") && !allowed.test(l),
    );
    if (errors.length) fail(`${s.name}: ${errors.join("; ")}`);
  }
}

// ---- queue2 (--q2) ----

const CAPS_Q2 = "caps join leave stats pause delay perf direct host results queue2";

// On the queue's own character select: up into the grid (that joins the panel), A on a character,
// then Start until Orca says the player is ready.
async function q2Ready(side, queue) {
  await side.waitScene("scSelctCharacter", 60000);
  await sleep(2500);
  if (PICK === "random") {
    // Up into the grid's bottom row, then right onto RANDOM (frames at 60 a second).
    await side.hold([128, 255, 128, 128], Math.round((RANDOM_PATH.up * 1000) / 60));
    await side.hold([255, 128, 128, 128], Math.round((RANDOM_PATH.right * 1000) / 60));
  } else if (PICK === "bowser") {
    await side.hold([128, 255, 128, 128], Math.round((BOWSER_PATH.up * 1000) / 60));
    await side.hold([255, 128, 128, 128], Math.round((BOWSER_PATH.right * 1000) / 60));
  } else {
    await side.hold([128, 255, 128, 128], 450);
  }
  await sleep(300);
  await side.press("A");
  await sleep(1500);
  for (let i = 0; i < COSTUME_PRESSES; ++i) {
    await side.press("X");
    await sleep(400);
  }
  for (let tries = 0; ; ++tries) {
    const at = side.lines.length;
    await side.press("START");
    const r = await side
      .expect(/^orca queue ready (casual|ranked) (\d+) (\d+)$/, 4000, at)
      .catch(() => null);
    if (r) {
      if (r.m[1] !== queue) fail(`${side.name}: ${r.line}, wanted ${queue}`);
      if (PICK === "random" && Number(r.m[2]) !== RANDOM_CHAR)
        fail(`${side.name}: ${r.line}, wanted Random (${RANDOM_CHAR})`);
      if (PICK === "bowser" && Number(r.m[2]) !== BOWSER_CHAR)
        fail(`${side.name}: ${r.line}, wanted Bowser (${BOWSER_CHAR})`);
      return r;
    }
    if (tries >= 4) fail(`${side.name}: Start on its own character select never readied it`);
  }
}

// The page's half (lib/orca-queue.ts with queue2): `orca menu online <q>` alone searches nothing;
// `orca queue ready` does, after the player's rating.
async function q2Search(side, queue, from) {
  const menu = await side.expect(/^orca menu online (casual|ranked)( local)?$/, 60000, from);
  if (menu.m[2]) fail(`${side.name}: "${menu.line}": Orca says local with host on`);
  if (menu.m[1] !== queue) fail(`${side.name}: picked ${menu.m[1]}, wanted ${queue}`);
  await sleep(1500);
  if (side.lines.slice(from).some((l) => l && l.startsWith("orca queue ready")))
    fail(`${side.name}: ready before the player pressed Start`);
  const ready = await q2Ready(side, queue);
  side.pick = [Number(ready.m[2]), Number(ready.m[3])];
  side.rating = queue === "ranked" ? (side.name === "ada" ? 1532 : 1488) : -1;
  side.send(`queue rating ${side.rating >= 0 ? side.rating : "-"}`);
  return search(side, queue);
}

// After its fresh start the host's own pick goes back on the room's select by its own inputs
// (ORCA.md "Fresh starts"), and only then may the opponent plug in: a session hands port 1 to the
// player's pad. The host's last fresh start must have put its pick back, in its colour, at a frame
// before the opponent's port starts. An Orca without fresh starts says none of this: skipped.
function ownPickBeforeSession(host) {
  if (!host.joinLog.some((e) => /^Drop-in: fresh start at frame /.test(e.line))) return;
  const back = host.queueLog.filter((l) => /port 1's own pick is /.test(l)).pop();
  const port2 = host.joinLog.filter((e) => /^Drop-in: port 2 is /.test(e.line)).pop();
  if (!back) fail(`${host.name}: no "port 1's own pick" line after its fresh start`);
  if (!port2) fail(`${host.name}: no "port 2 is" line`);
  const backFrame = Number(/frame (\d+)/.exec(back)[1]);
  const port2Frame = Number(/from frame (\d+)/.exec(port2.line)[1]);
  const m = /own pick is (back in|left to the player) \(character (0x[0-9a-f]+) costume (\d+)/.exec(back);
  log(`${host.name}: ${back}; ${port2.line}`);
  if (!m || m[1] !== "back in") fail(`${host.name}: its own pick wasn't put back: ${back}`);
  if (host.pick && Number(m[3]) !== host.pick[1])
    fail(`${host.name}: its own pick came back in costume ${m[3]}, its pick is ${host.pick[1]}`);
  if (backFrame >= port2Frame)
    fail(`${host.name}: port 2 plugged in at frame ${port2Frame}, before its own pick was back (${backFrame})`);
}

async function q2Meet(first, second, queue, pick) {
  const from = [first.lines.length, second.lines.length];
  await pick(first);
  const q1 = q2Search(first, queue, from[0]);
  await sleep(6000);
  await pick(second);
  const q2 = q2Search(second, queue, from[1]);
  const [m1, m2] = await Promise.all([q1, q2]);
  if (m1.room !== m2.room) fail(`matched into different rooms: ${m1.room} ${m2.room}`);
  const host = m1.slot === 0 ? first : second;
  const joiner = host === first ? second : first;
  log(`host ${host.name}, joiner ${joiner.name}, room ${m1.room}`);
  const hostFrom = host.lines.length;
  await Promise.all([hostOrJoin(first, m1.room, m1.slot), hostOrJoin(second, m2.room, m2.slot)]);
  await host.expect(/^orca state friend-joined$/, 120000, hostFrom);
  if (host.lines.slice(hostFrom).some((l) => l === "unsupported host")) fail("unsupported host");
  ownPickBeforeSession(host);
  return { host, joiner, room: m1.room };
}

// The room's character select: game 1, the joiner's pick goes into the host's game by itself; then
// both press Start (ready) until the game goes on. Ranked later games: the character order first.
async function q2Css(host, joiner, first, queueFrom = 0) {
  await host.waitScene("scSelctCharacter", 120000);
  if (first) {
    const at = Date.now();
    await waitFor(
      () => host.queueLog.slice(queueFrom).some((l) => /port 2's pick is in/.test(l)),
      30000,
      "the joiner's pick never went into the host's game",
    );
    const line = host.queueLog.slice(queueFrom).find((l) => /port 2's pick is in/.test(l));
    log(`the joiner's pick went in (${((Date.now() - at) / 1000).toFixed(1)} s on the host): ${line}`);
    if (PICK === "random" && !/character 0x29\b/.test(line))
      fail(`the joiner's Random pick went in as another character: ${line}`);
  }
  await sleep(1500);
  if (SCENARIO === "noskip") await noSkip([host, joiner]);
  if (QUEUE === "ranked" && !first) {
    await Promise.all([
      host.hold([128, 255, 128, 128], 450),
      joiner.hold([128, 255, 128, 128], 450),
    ]);
    await sleep(300);
  }
  const giga = GIGA && first;
  if (giga) {
    for (const s of [host, joiner]) s.pad.buttons |= BTN.L;
    log("--giga: both hold L through the character select's Start");
  }
  const altz = SCENARIO === "altz" && first;
  if (altz) {
    for (const s of [host, joiner]) s.pad.buttons |= BTN.Z;
    log("altz: both hold Z through the character select's go");
  }
  for (let tries = 0; host.scene === "scSelctCharacter"; ++tries) {
    if (tries >= (QUEUE === "ranked" ? 120 : 30)) fail("the character select never went on");
    if (QUEUE === "ranked" && !first && tries < 3) {
      await Promise.all([host.press("A"), joiner.press("A")]);
      await sleep(400);
    }
    await Promise.all([host.press("START"), joiner.press("START")]);
    await sleep(1200);
  }
  if (giga) for (const s of [host, joiner]) s.pad.buttons &= ~BTN.L;
  if (altz) {
    for (const s of [host, joiner]) s.pad.buttons &= ~BTN.Z;
    altzHeld = true;
  }
}

// --scenario noskip: ranked has no skip. On every character select both players hold B for 3 s
// (twice the casual skip's hold) and neither Orca may print `orca queue skip`.
async function noSkip(sides) {
  if (QUEUE !== "ranked") fail("noskip is a ranked scenario");
  const from = sides.map((s) => s.lines.length);
  for (const s of sides) s.pad.buttons |= BTN.B;
  await sleep(3000);
  for (const s of sides) s.pad.buttons &= ~BTN.B;
  await sleep(500);
  sides.forEach((s, i) => {
    if (s.lines.slice(from[i]).some((l) => l === "orca queue skip"))
      fail(`${s.name}: held B on a ranked character select and printed orca queue skip`);
  });
  ++noSkipHolds;
  log(`noskip: both held B 3 s on the ranked character select; no orca queue skip`);
}
let noSkipHolds = 0;
let altzHeld = false;

async function q2Fight(host, joiner) {
  if (QUEUE === "casual") {
    await host.waitScene("scMelee", 60000);
  } else {
    await host.waitScene("scSelStage", 60000);
    log("ranked stage select: waiting for the turn timers' defaults");
    await host.waitScene("scMelee", 200000);
  }
  await sleep(6000);
  if (CONTROLS[0] || CONTROLS_MID) {
    for (let i = 0; i < 3; ++i) {
      await host.press("Z");
      await sleep(900);
      await joiner.press("Z");
      await sleep(900);
    }
  }
  joiner.pad.axes = [0, 128, 128, 128];
  const hostFrom = host.lines.length;
  const setEndFrom = [host.setEndLog.length, joiner.setEndLog.length];
  const resultsFrom = [host.results.length, joiner.results.length];
  let chords = 0;
  const chorder =
    CHORD &&
    setInterval(() => {
      if (host.scene !== "scMelee") return;
      ++chords;
      host.chord();
      joiner.chord();
    }, 2500);
  await host
    .waitScene("scVsResult", 240000)
    .finally(() => {
      joiner.pad.axes = [128, 128, 128, 128];
      if (chorder) clearInterval(chorder);
    });
  if (CHORD) {
    await sleep(1500);
    await Promise.all([host.chord(), joiner.chord()]);
    log(`--chord: ${chords} chords by both in the fight, one each on the results screen`);
  }
  // The set's last game: VICTORY on one side, DEFEAT on the other (UX/SetEnd.h); then the room
  // after the set, until both left it.
  if (QUEUE === "ranked" && (await setEndPanels([host, joiner], setEndFrom, resultsFrom)))
    return setOverLeave(host, joiner, setEndFrom);
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

// The first "Set end: ..." line from `from` on that matches, within `ms`.
async function waitSetEnd(side, re, ms, from = 0) {
  const end = Date.now() + ms;
  for (;;) {
    const hit = side.setEndLog.slice(from).find((e) => re.test(e.line));
    if (hit) return hit;
    if (Date.now() > end) return null;
    await sleep(100);
  }
}

// On a ranked game's results screen: if the set ended, each side's panel shows VICTORY or DEFEAT
// and the room's verdict arrives. With --shots, also capture the panel as the rating lands.
async function setEndPanels(sides, from, resultsFrom) {
  const opened = await Promise.all(
    sides.map((s, i) => waitSetEnd(s, /^Set end: (VICTORY|DEFEAT) /, 4000, from[i])),
  );
  if (!opened.some((x) => x)) return false; // not the set's last game
  for (let i = 0; i < sides.length; ++i)
    if (!opened[i]) fail(`${sides[i].name}: no VICTORY or DEFEAT panel at the set's end`);
  const titles = opened.map((x) => /^Set end: (\w+)/.exec(x.line)[1]);
  if (titles[0] === titles[1]) fail(`both sides show ${titles[0]}`);
  log(`set end on screen: ${sides.map((s, i) => `${s.name} ${titles[i]}`).join(", ")}`);
  await Promise.all(
    sides.map(async (s, i) => {
      await sleep(400);
      s.shot(`${s.name}-set-open`);
      await waitFor(
        () => s.results.slice(resultsFrom[i]).some((r) => r.k === "set"),
        15000,
        `${s.name}: no verdict on the set`,
      );
      const verdict = Date.now();
      await sleep(Math.max(0, verdict + RATING_AFTER_MS + 450 - Date.now()));
      s.shot(`${s.name}-set-count`);
      await sleep(700);
      s.shot(`${s.name}-set-land`);
      await sleep(1600);
      s.shot(`${s.name}-set-settled`);
      if (!(await waitSetEnd(s, /^Set end: the page's rating/, 4000, from[i])))
        fail(`${s.name}: the page's new rating never reached the panel`);
    }),
  );
  return true;
}

// After a ranked set, both stay on the results screen until each leaves by pressing on, or
// (bleave) the joiner holds B and the host sees "<name> left" and leaves with Start. No forfeit
// countdown runs after the set.
async function setOverLeave(host, joiner, from) {
  const sides = [host, joiner];
  for (let i = 0; i < 2; ++i)
    if (!(await waitSetEnd(sides[i], /^Set end: set over/, 15000, from[i])))
      fail(`${sides[i].name}: the room never said the set is over`);
  // Together a while on the results screen: nobody leaves by itself.
  await sleep(3000);
  for (let i = 0; i < 2; ++i)
    if (sides[i].setEndLog.slice(from[i]).some((e) => /set over: leaving/.test(e.line)))
      fail(`${sides[i].name}: left the set's room before its player did anything`);
  if (host.scene !== "scVsResult") fail(`${host.name}: off the results screen by itself (${host.scene})`);
  for (const s of sides) s.shot(`${s.name}-set-over`);
  if (SCENARIO === "playagain") return playAgainLeave(host, joiner, from);
  if (SCENARIO === "bleave") {
    joiner.pad.buttons |= BTN.B;
    await sleep(900);
    joiner.shot(`${joiner.name}-set-over-hold`);
    const z = await waitSetEnd(joiner, /set over: leaving room \S+ \(B held\)/, 5000, from[1]).finally(
      () => (joiner.pad.buttons &= ~BTN.B),
    );
    if (!z) fail(`${joiner.name}: held B on the results screen and stayed in the room`);
    log(`${joiner.name}: ${z.line}`);
    const gone = await waitSetEnd(host, /^Set end: \S+ left the set's room/, 20000, from[0]);
    if (!gone) fail(`${host.name}: never heard ${joiner.name} leave the set's room`);
    log(`${host.name}: ${gone.line}`);
    await sleep(1200);
    host.shot(`${host.name}-set-over-left`);
    await sleep(1500);
    if (host.setEndLog.slice(from[0]).some((e) => /set over: leaving/.test(e.line)))
      fail(`${host.name}: left the set's room when ${joiner.name} did`);
    // Alone on its results screen: Start leaves.
    const presser = setInterval(() => host.press("START"), 900);
    try {
      const left = await waitSetEnd(host, /set over: leaving room/, 30000, from[0]);
      if (!left) fail(`${host.name}: Start never left the set's room`);
      log(`${host.name}: ${left.line}`);
    } finally {
      clearInterval(presser);
    }
  } else {
    // Both press on through the results screen, in the game's own flow.
    const presser = setInterval(() => {
      for (const s of sides) s.press(Date.now() % 2 ? "A" : "START");
    }, 900);
    try {
      for (const s of sides) {
        const left = await waitSetEnd(s, /set over: leaving room/, 60000, from[sides.indexOf(s)]);
        if (!left) fail(`${s.name}: never left the set's room`);
        log(`${s.name}: ${left.line}`);
      }
    } finally {
      clearInterval(presser);
    }
  }
  for (let i = 0; i < 2; ++i) {
    const log_ = sides[i].setEndLog.slice(from[i]);
    const at = log_.findIndex((e) => /^Set end: set over/.test(e.line));
    const bad = log_.slice(at).find((e) => /forfeit in|disconnected|unplugged after the stall/.test(e.line));
    if (bad) fail(`${sides[i].name}: a forfeit after the set: ${bad.line}`);
  }
}

// After the room: each side back on its own character select (the queue image), and `orca queue
// ready` again unless its own ready timer ran out.
async function q2Back(side, from, ready) {
  await waitFor(
    () => side.queueLog.some((l, i) => i >= from && /back on this player's own character select/.test(l)),
    30000,
    `${side.name}: never back on its own character select`,
  );
  const back = side.queueLog.slice(from).find((l) => /back on this player's own/.test(l));
  log(`${side.name}: ${back}`);
  if (ready) {
    await side.expect(/^orca queue ready (casual|ranked) (\d+) (\d+)$/, 15000, side.linesAtBack);
  } else {
    await sleep(5000);
    if (side.lines.slice(side.linesAtBack).some((l) => l && l.startsWith("orca queue ready")))
      fail(`${side.name}: ready again after its own ready timer ran out`);
  }
}

async function q2Main() {
  const a = new Side("ada", opt("--cookie-a"), opt("--id-a"));
  const b = new Side("bo", opt("--cookie-b"), opt("--id-b"));
  sides.push(a, b);
  for (const s of [a, b]) await s.startBridge();
  log(`work ${WORK}; ${SLUG ? `listing ${SLUG}` : `dev game ${DEV_GAME}`}; ${QUEUE}; q2 ${SCENARIO}`);
  a.start();
  await sleep(4000);
  b.start();
  for (const s of [a, b]) {
    await s.expect(/^orca caps .*\bhost\b/, 120000);
    if (!s.lines.some((l) => l && /^orca caps .*\bqueue2\b/.test(l)))
      fail(`${s.name}: Orca offers no queue2`);
    if (SCENARIO === "playagain" || SCENARIO === "cssjoin") {
      const offered = s.lines.find((l) => l && /^orca caps /.test(l)).slice(10).split(" ");
      s.send(`caps ${offered.filter((c) => PAGE_CAPS.has(c)).join(" ")}`);
    } else s.send(CAPS_Q2);
  }
  if (CONTROLS_MID) b.send(`controls ${CONTROLS_MID}`);
  if (SCENARIO === "cssjoin") return cssJoin(a, b);
  const { host, joiner, room } = await q2Meet(a, b, QUEUE, (s) => menuToQueue(s, QUEUE));
  const marks = () => {
    for (const s of [host, joiner]) {
      s.queueAt = s.queueLog.length;
      s.linesAtBack = s.lines.length;
    }
  };
  if (SCENARIO === "skip" || SCENARIO === "rematch") {
    if (SCENARIO === "rematch" && QUEUE !== "casual") fail("rematch is a casual scenario");
    await host.waitScene("scSelctCharacter", 120000);
    await waitFor(
      () => host.queueLog.some((l) => /port 2's pick is in/.test(l)),
      30000,
      "the joiner's pick never went into the host's game",
    );
    marks();
    const from = joiner.lines.length;
    joiner.pad.buttons |= BTN.B;
    await joiner.expect(/^orca queue skip$/, 6000, from).finally(() => {
      joiner.pad.buttons &= ~BTN.B;
    });
    joiner.send("leave");
    await host.expect(/^orca state friend-left /, 60000, host.linesAtBack);
    await Promise.all([q2Back(host, host.queueAt, true), q2Back(joiner, joiner.queueAt, true)]);
    noErrors([a, b]);
    if (SCENARIO === "skip") {
      const sums = checksumsMatched([a, b]);
      log(`PASS: skip: ${joiner.name} held B, left ${room}; both back on their own character select, ready; checksums: ${sums}`);
      return;
    }
    log(`skip: ${joiner.name} held B, left ${room}; both back on their own character select, ready`);
    // The quick rematch: both search again and, with dev tickets, meet in a new room. The skipper
    // waited longest, so it hosts from port 1.
    const sessionsBefore = [a.sessions.length, b.sessions.length];
    const s1 = search(joiner, QUEUE);
    await sleep(3000);
    const s2 = search(host, QUEUE);
    const [m1, m2] = await Promise.all([s1, s2]);
    if (m1.room !== m2.room) fail(`rematched into different rooms: ${m1.room} ${m2.room}`);
    if (m1.room === room) fail(`rematched into the skipped room ${room}`);
    const host2 = m1.slot === 0 ? joiner : host;
    const joiner2 = host2 === joiner ? host : joiner;
    log(`rematch: host ${host2.name}, joiner ${joiner2.name}, room ${m1.room}`);
    const host2From = host2.lines.length;
    const queueFrom = host2.queueLog.length;
    const results = [host2.results.length, joiner2.results.length];
    await Promise.all([
      hostOrJoin(joiner, m1.room, m1.slot),
      hostOrJoin(host, m2.room, m2.slot),
    ]);
    await host2.expect(/^orca state friend-joined$/, 120000, host2From);
    if (host2.lines.slice(host2From).some((l) => l === "unsupported host")) fail("unsupported host");
    await q2Css(host2, joiner2, true, queueFrom);
    await q2Fight(host2, joiner2);
    marks();
    joiner2.send("leave");
    await host2.expect(/^orca state friend-left /, 60000, host2.linesAtBack);
    await Promise.all([q2Back(host2, host2.queueAt, true), q2Back(joiner2, joiner2.queueAt, true)]);
    const n = sameVerdicts(host2, joiner2, results[0], results[1]);
    noErrors([a, b]);
    if (a.sessions.length < sessionsBefore[0] + 1 || b.sessions.length < sessionsBefore[1] + 1)
      fail("the rematch's session never ended on both sides");
    const sums = checksumsMatched([a, b], 2);
    log(`PASS: rematch: skipped ${room}, then ${host2.name} hosted ${m1.room}: ${n} verdict(s) the same on both sides; both back on their own character select, ready; checksums: ${sums}`);
    return;
  }
  if (SCENARIO === "rejoin") {
    if (QUEUE !== "ranked") fail("rejoin is a ranked scenario");
    marks();
    await q2Css(host, joiner, true);
    await host.waitScene("scSelStage", 60000);
    await host.waitScene("scMelee", 200000);
    await sleep(8000);
    const hostFrom = host.lines.length;
    const joinFrom = joiner.joinLog.length;
    const dropFrom = joiner.lines.length;
    joiner.send("test-drop-room");
    await joiner.expect(/^orca error network /, 30000, dropFrom);
    log(`${joiner.name}: its room connection dropped mid-fight`);
    await waitFor(
      () => joiner.queueLog.slice(joiner.queueAt).some((l) => /back on this player's own character select/.test(l)),
      30000,
      `${joiner.name} never went back to its own character select`,
    );
    // The page sends it back to the match's room.
    await sleep(JOIN_DELAY_MS);
    const back = joiner.lines.length;
    joiner.send(`join ${room}`);
    const r = await Promise.race([
      joiner.expect(/^orca state playing$/, 150000, back).then(() => "playing"),
      joiner.expect(/^orca error (\S+) (.*)$/, 150000, back).then((x) => `${x.m[1]}: ${x.m[2]}`),
    ]);
    if (r !== "playing") fail(`${joiner.name}: back in ${room}, then orca error ${r}`);
    await host.expect(/^orca state friend-joined$/, 60000, hostFrom);
    // Plugged in again, and both play on: 20 s with no error on either side.
    await sleep(20000);
    const joins = joiner.joinLog.slice(joinFrom).map((e) => e.line);
    const plugged = joins.filter((l) => /^Drop-in: plugged into port 2 at frame/.test(l));
    const ended = joins.filter((l) => /^Online: session ended/.test(l));
    log(`${joiner.name} after the drop: ${joins.filter((l) => /plugged into|session ended|joining room|port \d is/.test(l)).join(" | ")}`);
    if (plugged.length < 1) fail(`${joiner.name} never plugged in again`);
    if (ended.length !== 1) fail(`${joiner.name}: ${ended.length} session end(s) since the drop, wanted the drop's own`);
    noErrors([host]);
    noErrors([joiner], /^orca error network /);
    for (const s of [host, joiner]) if (s.desyncs.length) fail(`${s.name}: ${s.desyncs[0]}`);
    const sums = checksumsMatched([joiner]);
    log(`PASS: rejoin: ${joiner.name} dropped mid-fight, came back to ${room} and ${plugged[plugged.length - 1]}; 20 s on, no error on either side; ${joiner.name}'s first session: ${sums}`);
    return;
  }
  if (SCENARIO === "kill" || SCENARIO === "leave") {
    // kill: the joiner's Orca dies mid-set, like a crash. leave: the joiner's page leaves mid-set.
    // Either way the host plays on through the room's 15 s grace and wins the set by forfeit.
    if (QUEUE !== "ranked") fail(`${SCENARIO} is a ranked scenario`);
    const kill = SCENARIO === "kill";
    marks();
    await q2Css(host, joiner, true);
    if (IN_GAME === 2) {
      await q2Fight(host, joiner);
      await host.expect(/^orca result \{"q":"ranked","k":"game","n":1/, 60000);
      await q2Css(host, joiner, false);
    }
    await host.waitScene("scSelStage", 60000);
    await host.waitScene("scMelee", 200000);
    // The fighters are in about 4.7 s (Brawl, 283 frames) or 2.3 s (Project+, 140) after scMelee
    // starts; the game-start goes once that frame is final. --in-game 0 leaves 1 s in, during the
    // stage's loading image.
    await sleep(IN_GAME === 0 ? 1000 : 8000);
    // Each Orca told the room this game began, once, with the same id (the fight's first frame).
    const starts = [host, joiner].map((s) => s.gameStarts.map((g) => g.msg));
    log(`game-start sent: ${[host, joiner].map((s, i) => `${s.name} ${JSON.stringify(starts[i])}`).join("; ")}`);
    if (EXPECT === "void") {
      if (starts.some((x) => x.length)) fail("--expect void, but an Orca sent game-start");
    } else {
      for (let i = 0; i < 2; ++i) {
        if (starts[i].length !== IN_GAME)
          fail(`${[host, joiner][i].name}: ${starts[i].length} game-start(s) by game ${IN_GAME}`);
      }
      const ids = starts.map((x) => x.map((g) => g.id).join(","));
      if (ids[0] !== ids[1]) fail(`the game-start ids differ: ${ids[0]} / ${ids[1]}`);
      if (starts.flat().some((g) => !g.matchId || !/^g\d+$/.test(g.id)))
        fail(`a malformed game-start: ${JSON.stringify(starts)}`);
    }
    const seFrom = host.setEndLog.length;
    const joinerFrom = joiner.setEndLog.length;
    const killed = Date.now();
    if (kill) {
      joiner.proc.kill("SIGKILL");
      log(`${joiner.name}'s Orca killed mid-fight`);
    } else {
      joiner.send("leave");
      if (!(await waitSetEnd(joiner, /^Set end: this player left the set/, 5000, joinerFrom)))
        fail(`${joiner.name}: left the set with nothing on screen`);
      await sleep(900);
      joiner.shot("self-left");
    }
    // Before game 1 began the host counts down to no contest (an Orca older than 0.3.33, the void
    // control's, still says forfeit).
    const left = await waitSetEnd(
      host,
      EXPECT === "void"
        ? /left the room mid-set: (?:no contest|their forfeit) in (\d+) s/
        : /left the room mid-set: their forfeit in (\d+) s/,
      10000,
      seFrom,
    );
    if (!left) fail(`${host.name}: never counted ${joiner.name}'s ${EXPECT === "void" ? "no contest" : "forfeit"} down`);
    log(`${host.name} heard the leave ${left.at - killed} ms after the kill: countdown from 15 s`);
    await sleep(3000);
    host.shot("countdown");
    await sleep(5000);
    host.shot("countdown-late");
    const set = await host.expect(/^orca result (\{"q":"ranked","k":"set".*)$/, 40000);
    const r = JSON.parse(set.m[1]);
    if (EXPECT === "void" ? r.out !== "void" : r.out !== "won" || r.by !== "forfeit")
      fail(`the set: ${set.m[1]}`);
    log(`the set's verdict on ${host.name}: ${set.m[1]}`);
    const after = Date.now() - left.at;
    log(`the ${EXPECT} verdict came ${after} ms after the leave reached ${host.name} (the countdown said 15000)`);
    if (after < 14000 || after > 17500) fail(`the countdown was off by ${after - 15000} ms`);
    const broke = host.setEndLog.slice(seFrom).find((e) => /left the room while/.test(e.line));
    log(broke ? `the host's stall ended at the leave: ${broke.line}` : "the host wasn't stalled at the leave");
    await sleep(600);
    host.shot("forfeited");
    // The panel 2.5 s after the verdict, the page's rating 1.5 s after it: the count lands 3.5 s in.
    await sleep(3300);
    host.shot("victory-forfeit");
    await sleep(1500);
    host.shot("victory-forfeit-settled");
    // A void set rates nobody: the page sends no new rating.
    if (EXPECT === "forfeit" && !(await waitSetEnd(host, /^Set end: the page's rating/, 5000, seFrom)))
      fail(`${host.name}: the page's new rating never reached the panel`);
    // A leave at load: the leaver's screen said the set doesn't count, and its Orca never told the
    // room a game began, not even in the round trip before it was unplugged.
    if (IN_GAME === 0) {
      if (!kill && !joiner.setEndLog.slice(joinerFrom).some((e) => /left the set: before its first game/.test(e.line)))
        fail(`${joiner.name}: its screen didn't say the set doesn't count`);
      const late = [host, joiner].map((s) => s.gameStarts.length);
      if (late[1]) fail(`${joiner.name} sent game-start after its leave: ${JSON.stringify(joiner.gameStarts)}`);
      log(`game-start after the leave: ${host.name} ${late[0]}, ${joiner.name} ${late[1]}`);
    }
    // A set was played (game 1): back not ready.
    await q2Back(host, host.queueAt, false);
    if (!kill) await q2Back(joiner, joiner.queueAt, false);
    noErrors(kill ? [host] : [host, joiner]);
    const sums = checksumsMatched(kill ? [host] : [host, joiner]);
    log(`PASS: ${SCENARIO}: ${joiner.name} ${kill ? "died" : "left"} mid-set in game ${IN_GAME}${EXPECT === "forfeit" ? ` after both sent game-start ${starts[0].map((g) => g.id).join(",")}` : " (no game-start)"}; ${host.name} counted 15 s from the leave, the ${EXPECT} verdict came ${after} ms after it${EXPECT === "forfeit" ? ", VICTORY" : ""}; back on its own character select; checksums: ${sums}`);
    return;
  }
  if (SCENARIO === "timeout") {
    if (QUEUE !== "casual") fail("timeout is a casual scenario");
    marks();
    await Promise.all([
      host.expect(/^orca queue timeout me$/, 60000, host.linesAtBack),
      joiner.expect(/^orca queue timeout me$/, 60000, joiner.linesAtBack),
    ]);
    host.send("leave");
    joiner.send("leave");
    await Promise.all([q2Back(host, host.queueAt, false), q2Back(joiner, joiner.queueAt, false)]);
    noErrors([a, b]);
    const sums = checksumsMatched([a, b]);
    log(`PASS: timeout: nobody readied; both printed timeout me, left, and are back, not ready; checksums: ${sums}`);
    return;
  }
  // The match: game after game until the set (ranked) or --games (casual), then the joiner leaves.
  for (let g = 0; g < GAMES; ++g) {
    // A ranked set's room closes right after its last game: the way back starts during it.
    if (QUEUE === "ranked") marks();
    await q2Css(host, joiner, g === 0);
    await q2Fight(host, joiner);
    // The set is over once its verdict came or its room closed. A room that closed with no verdict
    // fails on the verdict check below.
    if (
      QUEUE === "ranked" &&
      (host.results.some((r) => r.k === "set") ||
        host.lines.slice(host.linesAtBack).includes("orca state friend-left match-over"))
    )
      break;
  }
  if (QUEUE !== "ranked") marks();
  if (QUEUE === "ranked") {
    await host.expect(/^orca result \{"q":"ranked","k":"set"/, 60000);
    await joiner.expect(/^orca result \{"q":"ranked","k":"set"/, 60000);
  } else {
    joiner.send("leave");
    await host.expect(/^orca state friend-left /, 60000, host.linesAtBack);
  }
  if (SCENARIO === "playagain") return playAgainInvite(host, joiner, a, b);
  // Casual searches again by itself; after a ranked set the player presses Start for that.
  const ready = QUEUE !== "ranked";
  await Promise.all([q2Back(host, host.queueAt, ready), q2Back(joiner, joiner.queueAt, ready)]);
  const n = sameVerdicts(host, joiner);
  noErrors([a, b]);
  const sums = checksumsMatched([a, b]);
  if (SCENARIO === "noskip") {
    if (!noSkipHolds) fail("noskip: B was never held");
  }
  if (SCENARIO === "altz") {
    if (!altzHeld) fail("altz: Z was never held through the go");
  }
  if (SCENARIO === "noskip" || SCENARIO === "altz") {
    for (const s of [a, b])
      if (s.lines.some((l) => l === "orca queue skip")) fail(`${s.name}: printed orca queue skip`);
  }
  // The characters each game was fought with (a Random pick resolved), from the host's verdicts.
  const fought = host.results
    .filter((r) => r.d && Array.isArray(r.d.c))
    .map((r) => r.d.c.join("/"))
    .join(", ");
  log(`PASS: q2 ${QUEUE}${SCENARIO !== "match" ? ` ${SCENARIO}` : ""}${PICK !== "character" ? ` pick ${PICK}` : ""}: ${n} verdicts the same on both sides${fought ? ` (characters ${fought})` : ""}; ${SCENARIO === "noskip" ? `B held on ${noSkipHolds} character select(s), no skip; ` : ""}${SCENARIO === "altz" ? "Z held through the go; " : ""}${QUEUE === "ranked" ? "set over: both stayed, then left; " : ""}both back on their own character select${ready ? ", ready" : ", not ready"}; checksums: ${sums}`);
}

// ---- --scenario playagain ----

let playAgain = null; // { inviter, invitee, room, how }

// Set over, per --variant: who leaves the set's room, and how. The one who stays stays on its
// results screen, in the room.
async function playAgainLeave(host, joiner, from) {
  const sides = [host, joiner];
  const byRole = (r) => (r === "host" ? host : joiner);
  let inviter = joiner;
  if (VARIANT === "b") inviter = host;
  else if ((VARIANT === "d" || VARIANT === "e") && INVITER) inviter = byRole(INVITER);
  const invitee = inviter === host ? joiner : host;
  const room = joiner.match?.room ?? host.match?.room;
  playAgain = { inviter, invitee, room, how: "" };
  const at = (s) => from[sides.indexOf(s)];
  if (VARIANT === "a" || VARIANT === "b") {
    inviter.pad.buttons |= BTN.B;
    const z = await waitSetEnd(inviter, /set over: leaving room \S+ \(B held\)/, 6000, at(inviter)).finally(
      () => (inviter.pad.buttons &= ~BTN.B),
    );
    if (!z) fail(`${inviter.name}: held B on the results screen and stayed in the room`);
    log(`${inviter.name} (inviter): ${z.line}`);
    await sleep(3000);
    if (invitee.setEndLog.slice(at(invitee)).some((e) => /set over: leaving/.test(e.line)))
      fail(`${invitee.name}: left the set's room when ${inviter.name} did`);
    playAgain.how = `${inviter.name} held B out of ${room}; ${invitee.name} stays there on its results screen`;
  } else if (VARIANT === "c") {
    inviter.send("leave");
    playAgain.how = `${inviter.name}'s page left ${room} (leave) with both still in it; ${invitee.name} stays`;
  } else {
    // d, e: both press on through the results screen, in the game's own flow (tonight's).
    const presser = setInterval(() => {
      for (const s of sides) s.press(Date.now() % 2 ? "A" : "START");
    }, 900);
    try {
      for (const s of sides) {
        const left = await waitSetEnd(s, /set over: leaving room/, 60000, at(s));
        if (!left) fail(`${s.name}: never left the set's room`);
        log(`${s.name}: ${left.line}`);
      }
    } finally {
      clearInterval(presser);
    }
    playAgain.how = `both pressed on out of ${room}`;
  }
  log(`play again: ${playAgain.how}; inviter ${inviter.name} (the set's ${inviter === host ? "host" : "joiner"}), invitee ${invitee.name}`);
}

// The invite and its acceptance; prints the RESULT line and exits 0 (joined) or 3 (failed).
async function playAgainInvite(host, joiner) {
  const pa = playAgain;
  if (!pa) fail("playagain: the set's room was never left (no set over?)");
  const { inviter, invitee, room } = pa;
  // The inviter: back on its own character select, not ready (a ranked set was played).
  await q2Back(inviter, inviter.queueAt, false);
  if (VARIANT === "e") {
    await inviter.press("B", 2500);
    await inviter.waitScene("muMenuMain", 30000);
    await sleep(2000);
    log(`${inviter.name}: backed out to the main menu (the control)`);
  }
  return inviteAndJoin(inviter, invitee, room, `playagain ${VARIANT} (${pa.how}; inviter ${inviter.name}, invitee ${invitee.name})`);
}

// The inviter's page invites the invitee into the room its Orca hosts (`prepare-join`), and the
// invitee's page accepts --invite-gap ms later (`join <code>`). RESULT: JOINED or JOIN-FAILED.
async function inviteAndJoin(inviter, invitee, room, name) {
  // Its own room, hosted (the page invites into the room its Orca is in: lib/orca-rooms.ts).
  await waitFor(
    () => {
      const m = inviter.mirror;
      return m && m.code && m.code !== room && m.me && m.host === m.me && (m.players || []).length >= 1;
    },
    30000,
    `${inviter.name}: no room of its own after the set`,
  );
  const code = inviter.mirror.code;
  const where = (s) => {
    const m = s.mirror;
    const inRoom = m && m.code && (m.players || []).length ? `room ${m.code}${m.host === m.me ? " (host)" : ""}` : "no room";
    return `${s.name}: ${inRoom}, scene ${s.scene}`;
  };
  log(`invite: ${where(inviter)}; ${where(invitee)}`);
  const fromInviter = inviter.lines.length;
  inviter.send("prepare-join");
  await sleep(SCENARIO === "playagain" && VARIANT === "c" ? 300 : INVITE_GAP_MS);
  log(`accept: ${where(invitee)}`);
  const fromInvitee = invitee.lines.length;
  const joinLogFrom = invitee.joinLog.length;
  const sent = Date.now();
  invitee.send(`join ${code}`);
  const ok = Promise.all([
    inviter.expect(/^orca state friend-joined$/, 120000, fromInviter),
    invitee.expect(/^orca state playing$/, 120000, fromInvitee),
  ]).then(() => null);
  const bad = Promise.race([
    invitee.expect(/^orca (error \S+.*|state (host-left|no-room|left).*)$/, 120000, fromInvitee).then((x) => `${invitee.name}: ${x.line}`),
    inviter.expect(/^orca state friend-left .*$/, 120000, fromInviter).then((x) => `${inviter.name}: ${x.line}`),
  ]);
  const why = await Promise.race([ok, bad]).catch((e) => `timeout: ${e.message}`);
  let verdict = why;
  if (!why) {
    // Plugged in: still together a while later.
    await sleep(8000);
    const gone =
      inviter.lines.slice(fromInviter).find((l) => l && /^orca state friend-left/.test(l)) ||
      invitee.lines.slice(fromInvitee).find((l) => l && /^orca (error|state (host-left|left))/.test(l));
    verdict = gone ? `left after plugging in: ${gone}` : null;
  } else {
    await sleep(3000);
  }
  const ms = Date.now() - sent;
  const joinLines = invitee.joinLog.slice(joinLogFrom).map((e) => `+${e.at - sent} ms ${e.line}`);
  log(`${invitee.name}'s join, from the join line on:`);
  for (const l of joinLines) log(`  ${l}`);
  const rules = invitee.joinLog.slice(joinLogFrom).find((e) => /Online rules: (the host's game says|leaving room)/.test(e.line));
  if (!verdict) {
    log(`RESULT: JOINED ${name}: ${invitee.name} plugged into ${inviter.name}'s room ${code}`);
    const marks = [inviter, invitee].map((s) => ({ sessions: s.sessions.length, desyncs: s.desyncs.length }));
    const plugged = invitee.joinLog
      .slice(joinLogFrom)
      .map((e) => /^Drop-in: plugged into port \d at frame (\d+)/.exec(e.line))
      .find(Boolean);
    const did = await stayTogether(inviter, invitee, fromInviter, fromInvitee);
    if (AFTER_LEAVE) {
      if (LEAVER === "invitee") await afterFriendLeaves(inviter, invitee, code);
      else await afterHostLeaves(inviter, invitee, code);
      await sessionGate([inviter, invitee], marks, plugged ? Number(plugged[1]) : null, did);
    }
    return;
  }
  log(`RESULT: JOIN-FAILED ${name}: ${verdict}${rules ? ` | ${rules.line}` : ""} (${ms} ms after the join)`);
  joinFailed = true;
}
let joinFailed = false;
let homeFailed = false;
let togetherFailed = false;
let rearmFailed = false;

// The friend leaves the friends' game (`leave`): the host, alone again, gets its queue's pick armed
// again (RearmFriendsPick), and the friend goes home to a room of its own.
async function afterFriendLeaves(host, friend, code) {
  const fromHost = host.lines.length;
  const fromFriend = friend.lines.length;
  const sent = Date.now();
  friend.send("leave");
  await host.expect(/^orca state friend-left/, 30000, fromHost);
  const rearm = await host.expect(/^orca menu online (ranked|casual)$/, 20000, fromHost).catch(() => null);
  let home = null;
  for (const end = Date.now() + 20000; Date.now() < end; await sleep(250)) {
    const m = friend.mirror;
    if (m && m.code && m.code !== code && m.me && m.host === m.me && (m.players || []).length) {
      home = m.code;
      break;
    }
  }
  const noRoom = friend.lines.slice(fromFriend).find((l) => l && /^orca state no-room/.test(l));
  const hostRoom = host.mirror && host.mirror.host === host.mirror.me ? host.mirror.code : null;
  if (rearm)
    log(`RESULT: REARMED ${host.name}: ${rearm.line} ${Date.now() - sent} ms after ${friend.name} left; still hosts ${hostRoom ?? "no room"} on ${host.scene}`);
  else {
    rearmFailed = true;
    const lines = host.lines.slice(fromHost).filter((l) => l && /^orca (menu|state|queue)/.test(l));
    log(`RESULT: NOT-REARMED ${host.name} after ${friend.name} left (scene ${host.scene}, room ${hostRoom ?? "none"}): ${lines.join(" | ")}`);
  }
  if (home && !noRoom) log(`RESULT: HOME ${friend.name} hosts its own room ${home} after leaving ${code}`);
  else {
    homeFailed = true;
    log(`RESULT: NO-ROOM ${friend.name} after leaving ${code}: ${noRoom ?? "no room of its own in 20 s"}; scene ${friend.scene}`);
  }
}

// Joined: --together ms in the new room (--fight: a game played there in that time), nobody gone.
async function stayTogether(inviter, invitee, fromInviter, fromInvitee) {
  const t0 = Date.now();
  let did = `${Math.round(TOGETHER_MS / 1000)} s together`;
  if (FIGHT) did = await friendsGame(inviter, invitee);
  const rest = TOGETHER_MS - (Date.now() - t0);
  if (rest > 0) await sleep(rest);
  const gone =
    inviter.lines.slice(fromInviter).find((l) => l && /^orca state friend-left/.test(l)) ||
    invitee.lines.slice(fromInvitee).find((l) => l && /^orca (error|state (host-left|left|no-room))/.test(l));
  if (gone) {
    togetherFailed = true;
    log(`RESULT: TOGETHER-FAILED apart before the inviter left: ${gone}`);
  }
  return `${did}, ${((Date.now() - t0) / 1000).toFixed(0)} s in all`;
}

// A game in the friends' room (page-e2e.mjs playGame): both pick, the inviter (port 1) starts and
// takes a stage, port 2 walks off the stage until the results, both press on to the select. Port 2
// (the friend, a fresh hand on its panel) picks as on a fresh select: A, up into the grid, A. Port
// 1's hand is wherever its last pick left it (after a ranked set: the grid's top row, where up
// reaches the BACK bar), so it first tries Start as it is, then A in place, and only on a fresh
// select (its hand still on its panel) the same A, up, A.
async function friendsGame(host, joiner) {
  try {
    await host.waitScene("scSelctCharacter", 15000);
  } catch {
    log(`friends game: ${host.name} is not on a character select (${host.scene}); no game`);
    return `no game (${host.name} on ${host.scene})`;
  }
  await sleep(2500);
  await joiner.press("A");
  await sleep(600);
  await joiner.hold([128, 255, 128, 128], 450);
  await sleep(300);
  await joiner.press("A");
  await sleep(1500);
  const leftCss = async (n) => {
    for (let i = 0; i < n && host.scene === "scSelctCharacter"; ++i) {
      await host.press("START");
      await sleep(1200);
    }
    if (host.scene === "muMenuMain") fail(`friends game: ${host.name} went back to the main menu`);
    return host.scene !== "scSelctCharacter";
  };
  let how = "Start as it was";
  if (!(await leftCss(3))) {
    how = "A in place, then Start";
    await host.press("A");
    await sleep(800);
    if (!(await leftCss(3))) {
      how = "A, up, A, then Start";
      await host.press("A");
      await sleep(600);
      await host.hold([128, 255, 128, 128], 450);
      await sleep(300);
      await host.press("A");
      await sleep(1500);
      if (!(await leftCss(20))) fail(`friends game: ${host.name}'s Start never left the character select`);
    }
  }
  log(`friends game: ${host.name} picked (${how})`);
  log(`friends game: the character select went on (${host.scene})`);
  await host.waitScene("scSelStage", 30000);
  await sleep(1500);
  await host.hold([128, 255, 128, 128], PPLUS ? 70 : 700);
  await host.press("A");
  for (let i = 0; i < 6 && host.scene === "scSelStage"; ++i) {
    await sleep(1500);
    await host.press("A");
  }
  await host.waitScene("scMelee", 30000);
  const fightAt = Date.now();
  log(`friends game: fighting`);
  await sleep(6000);
  joiner.pad.axes = [0, 128, 128, 128];
  await host.waitScene("scVsResult", 300000).finally(() => (joiner.pad.axes = [128, 128, 128, 128]));
  const fought = Math.round((Date.now() - fightAt) / 1000);
  log(`friends game: results after ${fought} s of fight`);
  await sleep(4000);
  const presser = setInterval(() => {
    if (host.scene !== "scVsResult") return;
    host.press(Date.now() % 2 ? "A" : "START");
    joiner.press("A");
  }, 900);
  try {
    await host.waitScene("scSelctCharacter", 60000);
  } finally {
    clearInterval(presser);
  }
  await sleep(1500);
  return `a game played (${fought} s of fight), back on the select`;
}

// After the inviter's leave: each side's session from the join on matched checksums over 15 s or
// more of frames, and no desync in either log since the join.
async function sessionGate(sides, marks, pluggedAt, did) {
  for (const end = Date.now() + 20000; Date.now() < end; await sleep(250))
    if (sides.every((s, i) => s.sessions.length > marks[i].sessions)) break;
  const parts = [];
  let bad = null;
  sides.forEach((s, i) => {
    const desync = s.desyncs.slice(marks[i].desyncs)[0];
    const ended = s.sessions.slice(marks[i].sessions);
    const last = ended[ended.length - 1];
    if (desync) bad ??= `${s.name}: ${desync}`;
    if (!last) {
      bad ??= `${s.name}: no session end after the join`;
      return;
    }
    const frames = pluggedAt === null ? null : last.frame - pluggedAt;
    parts.push(`${s.name} ${last.checksums} checksums matched over ${frames ?? "?"} frames (${last.why}, ${last.rollbacks} rollbacks)`);
    if (last.checksums <= 0) bad ??= `${s.name}: the session matched no checksums (${last.why})`;
    if (frames === null) bad ??= `${s.name}: no plug-in frame in the invitee's log`;
    else if (frames < 900) bad ??= `${s.name}: ${frames} frames together, under 15 s`;
  });
  if (bad) {
    togetherFailed = true;
    log(`RESULT: TOGETHER-FAILED ${bad}; ${parts.join("; ")}`);
    return;
  }
  log(`RESULT: TOGETHER ${did}; ${parts.join("; ")}`);
}

// The friends' game after Play again: its host (the inviter) leaves, and the invitee, alone on port
// 2 of the inviter's game, goes home on the menus to port 1 and a room of its own within 20 s, with
// no `orca state no-room` on the way.
async function afterHostLeaves(inviter, invitee, code) {
  const from = invitee.lines.length;
  const sent = Date.now();
  inviter.send("leave");
  await invitee.expect(/^orca state host-left/, 30000, from);
  let home = null;
  const end = Date.now() + 20000;
  while (Date.now() < end) {
    const m = invitee.mirror;
    if (m && m.code && m.code !== code && m.me && m.host === m.me && (m.players || []).length) {
      home = m.code;
      break;
    }
    await sleep(250);
  }
  const noRoom = invitee.lines.slice(from).find((l) => l && /^orca state no-room/.test(l));
  if (home && !noRoom) {
    log(`RESULT: HOME ${invitee.name} hosts its own room ${home} ${Date.now() - sent} ms after ${inviter.name} left`);
    return;
  }
  homeFailed = true;
  log(`RESULT: NO-ROOM ${invitee.name} after ${inviter.name} left: ${noRoom ?? "no room of its own in 20 s"}; scene ${invitee.scene}`);
}

// --scenario cssjoin (--q2): the shortest path to where a ranked set leaves the inviter: on the
// queue's own character select (With Anyone > Ranked or Casual, not searching), hosting its own
// private room. ada invites, bo (on its main menu) accepts. --where ranked|casual (the queue's own
// character select), menu (ada stays on the main menu: the control).
async function cssJoin(a, b) {
  const where = opt("--where", "ranked");
  const mash = (side) =>
    setInterval(() => {
      if (["", "scStrap", "scBoot", "scTitle"].includes(side.scene)) side.press("A");
    }, 700);
  const mb = mash(b);
  if (where === "menu") {
    const ma = mash(a);
    await a.waitScene("muMenuMain", 120000);
    clearInterval(ma);
  } else {
    const from = a.lines.length;
    await menuToQueue(a, where);
    const menu = await a.expect(/^orca menu online (casual|ranked)( local)?$/, 60000, from);
    if (menu.m[1] !== where) fail(`${a.name}: picked ${menu.m[1]}, wanted ${where}`);
    await a.waitScene("scSelctCharacter", 60000);
  }
  await b.waitScene("muMenuMain", 120000);
  clearInterval(mb);
  await sleep(4000);
  log(`${a.name} on ${where === "menu" ? "the main menu" : `the ${where} queue's own character select (not searching)`}`);
  return inviteAndJoin(a, b, null, `cssjoin ${where} (inviter ${a.name}, invitee ${b.name})`);
}

// Back out of the character select to the With Anyone page (the cursor comes back on the pick),
// then pick it again.
async function pickAgain(side) {
  await side.press("B", 2500);
  await side.waitScene("muMenuMain", 30000);
  await sleep(3000);
  await side.press("A");
}

async function main() {
  if (Q2) return q2Main();
  if (SCENARIO === "cancel") return cancelScenario();
  if (SCENARIO === "noshow") return noShowScenario();
  const a = new Side("ada", opt("--cookie-a"), opt("--id-a"));
  const b = new Side("bo", opt("--cookie-b"), opt("--id-b"));
  sides.push(a, b);
  for (const s of [a, b]) await s.startBridge();
  log(
    `work ${WORK}; ${SLUG ? `listing ${SLUG} on ${SITE}` : `dev game ${DEV_GAME}`}; ${QUEUE}; ${SCENARIO}`,
  );
  a.start();
  await sleep(4000);
  b.start();
  for (const s of [a, b]) {
    await s.expect(/^orca caps .*\bhost\b/, 120000);
    if (QUEUE === "ranked" && !s.lines.some((l) => l && /^orca caps .*\bresults\b/.test(l)))
      fail(`${s.name}: Orca offers no results for this profile`);
    s.send("caps join leave stats pause delay perf direct host results");
  }
  const { host, joiner, room } = await meet(a, b, QUEUE, (s) => menuToQueue(s, QUEUE));

  // A host with its opponent plugged in refuses another `host`.
  {
    const from = host.lines.length;
    host.send("host zz22zz22");
    await host.expect(/^unsupported host$/, 10000, from);
  }

  if (SCENARIO === "forfeit") {
    if (QUEUE !== "ranked") fail("forfeit is a ranked scenario");
    await playGame(host, joiner, true);
    await host.expect(/^orca result \{"q":"ranked","k":"game","n":1/, 30000);
    // The joiner's connection drops mid-set (as a network failure): the room awards the set to the
    // host after 15 s, which waits in the room for it.
    joiner.send("test-drop-room");
    const set = await host.expect(/^orca result (\{"q":"ranked","k":"set".*)$/, 60000);
    const r = JSON.parse(set.m[1]);
    if (r.out !== "won" || r.by !== "forfeit") fail(`the set: ${set.m[1]}`);
    await host.expect(/^orca state friend-left match-over$/, 30000);
    noErrors([host], /^$/);
    log("PASS: the opponent dropped mid-set: the host won the set by forfeit");
    return;
  }

  const wantGames = QUEUE === "ranked" ? 2 : SCENARIO === "rehost" ? 1 : GAMES;
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
    if (!host.ownRooms.some((r) => r !== room))
      fail("the host opened no room of its own after the set");
  } else {
    const from = host.lines.length;
    joiner.send("leave");
    await host.expect(/^orca state friend-left /, 60000, from);
  }
  const n = sameVerdicts(host, joiner);
  noErrors([a, b]);

  if (SCENARIO === "rehost") {
    // The former joiner (port 2, no room of its own) queues first this time: it hosts, moving to
    // port 1, and the former host joins it.
    await sleep(3000);
    const hostResults = joiner.results.length,
      joinerResults = host.results.length;
    const next = await meet(joiner, host, QUEUE, pickAgain);
    if (next.host !== joiner) fail("the former joiner did not get slot 0");
    await playGame(next.host, next.joiner, true);
    await next.host.expect(/^orca result \{"q":"casual","k":"game","n":1/, 60000);
    await next.joiner.expect(/^orca result \{"q":"casual","k":"game","n":1/, 60000);
    sameVerdicts(next.host, next.joiner, hostResults, joinerResults);
    noErrors([a, b]);
    log(`PASS: ${n} verdicts, then the former joiner hosted the next match from port 1`);
    return;
  }
  log(`PASS: ${n} verdicts, the same on both sides`);
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
if (SHOTS_OUT) {
  fs.mkdirSync(SHOTS_OUT, { recursive: true });
  for (const s of sides) {
    for (const name of s.shots) {
      const from = path.join(WORK, `user-${s.name}`, "ScreenShots", `${name}.png`);
      if (fs.existsSync(from)) fs.copyFileSync(from, path.join(SHOTS_OUT, `${name}.png`));
      else log(`no screenshot ${from}`);
    }
  }
  log(`screenshots in ${SHOTS_OUT}`);
}
if (!KEEP && code === 0 && !joinFailed && !homeFailed && !togetherFailed && !rearmFailed) fs.rmSync(WORK, { recursive: true, force: true });
else log(`logs in ${WORK}`);
process.exit(
  code !== 0 ? code : joinFailed ? 3 : homeFailed ? 4 : togetherFailed ? 5 : rearmFailed ? 6 : 0,
);
