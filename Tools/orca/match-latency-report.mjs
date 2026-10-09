#!/usr/bin/env node
// match-latency-report.mjs: cuts match-latency.mjs runs into per-match phase timelines, prints
// p50/p90 per phase, and fails a run that shows a broken join (ORCA.md, "Match start timing").
//
//   node match-latency-report.mjs [--json out.json] [--csv out.csv] [--detail] [--label <text>]
//     [--max-p50 <ms>] [--max-p90 <ms>] [--min-ok <n>] <run dir> [<run dir> ...]
//
// A run dir holds one or both sides (<name>.raw.jsonl + meta.json). Sides from separate dirs (one
// per machine) are paired by room code; a second machine's clock is moved onto the first's by the
// median gap between the two sides' `matched`. Every phase is reported in ms; t = 0 is the moment
// the page learned of the match (the /match socket's `matched`), the earlier of the two sides'.
//
// Besides the phases it counts, per match, the replay's way over (inline in the room's offer, or a
// store PUT and GET) and its size, and it fails the run (exit 1) on any match whose logs show a
// broken join: a replay that didn't reproduce the host's game (room_mismatch), `orca error
// mismatch`, a store refusal for a seat not yet listed (not_seated), the host's header never landing
// (FailHeaderWait), a live frame moved or history past it, or a desync. --max-p50/--max-p90/--min-ok
// add a gate on "matched -> both in control".
import fs from "node:fs";
import path from "node:path";

const argv = process.argv.slice(2);
const opt = (n) => {
  const i = argv.indexOf(n);
  if (i < 0) return null;
  const v = argv[i + 1];
  argv.splice(i, 2);
  return v;
};
const flag = (n) => {
  const i = argv.indexOf(n);
  if (i < 0) return false;
  argv.splice(i, 1);
  return true;
};
const JSON_OUT = opt("--json");
const CSV_OUT = opt("--csv");
const DETAIL = flag("--detail");
const LABEL = opt("--label");
// A regression gate: the total "matched -> both in control" must stay under these (ms), with at
// least --min-ok complete matches; exit 1 otherwise.
const MAX_P50 = Number(opt("--max-p50") ?? 0);
const MAX_P90 = Number(opt("--max-p90") ?? 0);
const MIN_OK = Number(opt("--min-ok") ?? 0);
const dirs = argv;

// Orca's log stamp is mm:ss:mmm of the wall clock; its hour comes from the line's arrival.
function stampToEpoch(stamp, arrival) {
  const m = /^(\d\d):(\d\d):(\d\d\d) /.exec(stamp);
  if (!m) return null;
  const d = new Date(arrival);
  d.setMinutes(Number(m[1]), Number(m[2]), Number(m[3]));
  let t = d.getTime();
  if (t - arrival > 30 * 60e3) t -= 3600e3;
  if (arrival - t > 30 * 60e3) t += 3600e3;
  return t;
}

function loadSide(dir, file, meta) {
  const name = file.replace(/\.raw\.jsonl$/, "");
  // Cloudflare's trace `ts` proved too coarse to align clocks (the offset moved by hundreds of ms
  // between samples), so each machine keeps its own clock; pairs across machines align on `matched`.
  const off = 0;
  const rows = [];
  for (const line of fs
    .readFileSync(path.join(dir, file), "utf8")
    .split("\n")) {
    if (!line) continue;
    let r;
    try {
      r = JSON.parse(line);
    } catch {
      continue;
    }
    // Windows Orca logs to stdout: a stamped stdout line is a log line.
    if (r.k === "o" && /^\d\d:\d\d:\d\d\d /.test(r.s || "")) r.k = "e";
    if (r.k === "e") {
      const ot = stampToEpoch(r.s, r.t);
      // Use Orca's own stamp when it is close to the arrival (it is the more exact one).
      r.ot = ot !== null && Math.abs(ot - r.t) < 5000 ? ot : r.t;
    } else r.ot = r.t;
    r.et = r.ot + off; // on the edge clock
    rows.push(r);
  }
  return { name, dir, meta, rows, off };
}

// Each side's matches: from its `matched` until its next `search` (or the end).
function splitMatches(side) {
  const out = [];
  const idx = side.rows
    .map((r, i) => (r.k === "b" && r.ev === "matched" ? i : -1))
    .filter((i) => i >= 0);
  for (const [j, i] of idx.entries()) {
    const m = side.rows[i];
    let end = side.rows.length;
    for (let k = i + 1; k < side.rows.length; ++k) {
      const r = side.rows[k];
      if (
        r.k === "b" &&
        (r.ev === "search" || (r.ev === "matched" && k !== i))
      ) {
        end = k;
        break;
      }
    }
    if (j + 1 < idx.length) end = Math.min(end, idx[j + 1]);
    out.push({
      side: side.name,
      n: m.n,
      room: m.room,
      slot: m.slot,
      friends: !!m.friends,
      rows: side.rows.slice(i, end),
      t0: m.et,
      meta: side.meta,
    });
  }
  return out;
}

const firstRow = (rows, pred) => rows.find(pred);
const fe = (rows, re) => {
  for (const r of rows) {
    if (r.k !== "e") continue;
    const m = re.exec(r.s);
    if (m) return { t: r.et, m };
  }
  return null;
};
const fo = (rows, re) => {
  for (const r of rows) {
    if (r.k !== "o") continue;
    const m = re.exec(r.s);
    if (m) return { t: r.et, m };
  }
  return null;
};
const fb = (rows, ev, pred = () => true) => {
  const r = rows.find((x) => x.k === "b" && x.ev === ev && pred(x));
  return r ? { t: r.et, r } : null;
};

// Lines that mean a broken join or session.
const FAILURE_RE =
  /replay didn't reproduce the same game|^orca error (room_mismatch|mismatch)\b|not_seated|not seated in that room|this game's header isn't the room's after|moved this player's live frame|past this player's live frame|Desync at frame/;

// The events of one side of one match.
function sideEvents(sm) {
  const R = sm.rows;
  const e = {};
  e.matched = sm.t0;
  const cmd = fb(R, "cmd", (x) => /^(host|join) /.test(x.s));
  e.cmd = cmd?.t;
  e.cmd_rx = fe(
    R,
    /Orca: joining a YouGame room \((as the host|to join a friend's game)\)/,
  )?.t;
  e.queue_image = fe(
    R,
    /Queue: this player's own game kept at frame \d+ \((\d+) ms/,
  );
  e.old_room_end = fe(R, /Orca room ended: You left the room/)?.t;
  const tq = fb(R, "ticket_req", (x) => x.matched || sm.friends);
  e.ticket_req = tq?.t;
  const th = fb(R, "ticket_http", (x) => x.kind === "match_room" || sm.friends);
  e.ticket_http_ms = th?.r.ms;
  e.ticket_ok = fb(R, "ticket_ok", (x) => x.matched || sm.friends)?.t;
  // The matched room's welcome (by its code: an own room reopening after the last match can
  // interleave), and the socket that opened just before it.
  const wel = fe(
    R,
    new RegExp(
      `Orca room: in room ${sm.room} on seat (\\d), (hosting|joining)`,
    ),
  );
  e.welcome = wel?.t;
  if (wel) wel.m = [wel.m[0], sm.room, wel.m[1], wel.m[2]];
  e.ws_open = R.filter(
    (r) =>
      r.k === "e" &&
      /Orca room: TEST network delay/.test(r.s) &&
      r.et <= (e.welcome ?? Infinity),
  )
    .map((r) => r.et)
    .pop();
  e.role = wel
    ? wel.m[3] === "hosting"
      ? "host"
      : "join"
    : sm.slot === 0
      ? "host"
      : "join";
  e.peer_here = fe(R, /Orca: Room \w+: .+ is here/)?.t; // joiner: the host's hello
  e.arrived = fe(R, /Drop-in: .+ arrived for port \d/)?.t; // host: the joiner's hello
  e.fresh_armed = fe(R, /a fresh start at the next boundary/)?.t;
  const fr = fe(
    R,
    /Drop-in: fresh start at frame \d+: back to the origin \(frame \d+\) in (\d+) ms/,
  );
  e.fresh_done = fr?.t;
  e.fresh_restore_ms = fr ? Number(fr.m[1]) : undefined;
  const tail = fe(
    R,
    /Drop-in: the fresh start's tail ran frames (\d+)-(\d+) in (\d+) ms/,
  );
  e.tail_done = tail?.t;
  e.tail_ms = tail ? Number(tail.m[3]) : undefined;
  e.tail_frames = tail ? Number(tail.m[2]) - Number(tail.m[1]) + 1 : undefined;
  const pb = fe(
    R,
    /Drop-in: port 1's pick is back at frame (\d+) \((\d+) ms after the fresh start\)/,
  );
  e.own_pick_back = pb?.t;
  const kf = fe(R, /Drop-in: keyframe of frame (\d+):/);
  e.kf_done = kf?.t;
  e.kf_frame = kf ? Number(kf.m[1]) : undefined;
  // How the replay went over: "<n> bytes inline in the room's offer" or "<n> bytes stored" (older
  // builds: "<x> MB stored", always the store).
  const kfs = fe(R, /Drop-in: keyframe of frame \d+: .*?(\d+) bytes (inline|stored)/);
  e.kf_bytes = kfs ? Number(kfs.m[1]) : undefined;
  e.kf_inline = kfs ? kfs.m[2] === "inline" : kf ? false : undefined;
  // The fresh start's unseen hold ends (builds with it): the host's picture is back.
  e.shown = fe(R, /Drop-in: the fresh start shows again at frame \d+/)?.t;
  e.host_session = fe(
    R,
    /Drop-in: port \d replays from keyframe frame (\d+), live from frame (\d+)/,
  )?.t;
  const off = fe(
    R,
    /Drop-in: the host offers keyframe \S+ \(frame (\d+), (?:([\d.]+) MB|(\d+) bytes, (inline|from the store))\)/,
  );
  e.offer_rx = off?.t;
  // Older builds log "0.0 MB" for any replay under 50 KB: unknown, not 0.
  e.offer_bytes =
    off?.m[3] != null
      ? Number(off.m[3])
      : off && Number(off.m[2]) > 0
        ? Math.round(Number(off.m[2]) * 1e6)
        : undefined;
  e.offer_inline = off ? off.m[4] === "inline" : undefined;
  // A broken join or session, on either side (the run fails on any).
  e.failures = R.filter(
    (r) =>
      (r.k === "e" || r.k === "o") &&
      FAILURE_RE.test(r.s || ""),
  ).map((r) => r.s.replace(/^.*?: (?=[A-Z])/, "").slice(0, 160));
  e.dl_done = fo(R, /^orca state joining (5\d|[6-9]\d)$/)?.t; // 50 = download complete
  {
    const js = R.filter(
      (r) => r.k === "o" && /^orca state joining \d+$/.test(r.s),
    );
    e.last_joining = js.length ? js[js.length - 1].et : undefined;
  }
  e.rebuild_menu = fe(
    R,
    /Online menu: frame \d+: a fresh start leaves the main menu/,
  )?.t;
  const rb = fe(R, /Drop-in: rebuilt replay frame (\d+) locally in (\d+) ms/);
  e.rebuilt = rb?.t;
  e.rebuild_ms = rb ? Number(rb.m[2]) : undefined;
  const pl = fe(
    R,
    /Drop-in: plugged into port (\d) at frame (\d+): catch-up (\d+) ms, (\d+) ms since the keyframe was offered/,
  );
  e.plugged = pl?.t;
  e.catchup_ms = pl ? Number(pl.m[3]) : undefined;
  e.friend_joined = fo(R, /^orca state friend-joined$/)?.t;
  e.state_playing = fo(R, /^orca state playing$/)?.t;
  e.p2_pick_in = fe(R, /Queue: frame \d+: port 2's pick is in/)?.t;
  e.lockin = fe(
    R,
    /rules bar '[^']*(Press Start to lock in|Start locks it in)/,
  )?.t;
  const dlu = fe(R, /Orca direct link: seat \d up, ([^,(]+)/);
  e.dl_up = dlu?.t;
  e.dl_kind = dlu?.m[1]?.trim();
  e.dl_start = fe(R, /Orca direct link: seat \d: (offering|answering) link/)?.t;
  const st = fe(
    R,
    /Online match at frame \d+: frame \d+, (\d+) rollbacks, (\d+) re-run frames, deepest \d+, (\d+) stalls/,
  );
  e.first_stats_stalls = st ? Number(st.m[3]) : undefined;
  e.endpoint = fb(R, "endpoint")?.t;
  e.error = fb(R, "match_error")?.r.error;
  // What the player reads on the select's rules bar, in order (Orca's native text).
  e.bars = R.filter((r) => r.k === "e" && /rules bar '/.test(r.s)).map((r) => ({
    t: r.et,
    bar: /rules bar '([^']*)'/.exec(r.s)[1],
  }));
  // stdout states (joining N, friend-joining, playing ...)
  e.states = R.filter(
    (r) => r.k === "o" && /^orca (state|error) /.test(r.s),
  ).map((r) => ({ t: r.et, s: r.s }));
  e.tx = (() => {
    const s = R.filter((r) => r.k === "o" && r.s.startsWith("orca stats "))
      .map((r) => /\btx[= ](\d)/.exec(r.s)?.[1])
      .filter(Boolean);
    return s.length ? s[s.length - 1] : undefined;
  })();
  return e;
}

const d = (a, b) => (a != null && b != null ? Math.round(b - a) : null);

// One match, both sides: the phase list.
function phases(host, join, friends) {
  const t0 = Math.min(host.matched ?? Infinity, join.matched ?? Infinity);
  const P = {};
  // Clock check: both sides hear `matched` from the same queue at the same moment (Mac<->PC: after the edge-clock fix).
  P["clock check: join matched - host matched"] = d(host.matched, join.matched);
  // The page.
  P["host: matched -> host cmd"] = d(host.matched, host.cmd);
  P["join: matched -> join cmd"] = d(join.matched, join.cmd);
  // Host: getting into the matched room.
  P["host: cmd -> Orca has it"] = d(host.cmd, host.cmd_rx);
  P["host: cmd -> old room closed"] = d(host.cmd_rx, host.old_room_end);
  P["host: ticket mint (HTTP)"] = host.ticket_http_ms ?? null;
  P["host: cmd -> ticket asked"] = d(host.cmd_rx, host.ticket_req);
  P["host: ticket -> socket open"] = d(host.ticket_ok, host.ws_open);
  P["host: socket open -> welcome"] = d(host.ws_open, host.welcome);
  P["host: cmd -> welcome (total)"] = d(host.cmd_rx, host.welcome);
  // Host: the fresh start.
  P["host: welcome -> fresh start restored"] = d(host.welcome, host.fresh_done);
  P["host: restore (Orca's ms)"] = host.fresh_restore_ms ?? null;
  P["host: tail (Orca's ms)"] = host.tail_ms ?? null;
  P["host: joiner's hello -> fresh start restored"] = friends
    ? d(host.arrived, host.fresh_done)
    : null;
  P["host: tail done -> own pick back"] = d(host.tail_done, host.own_pick_back);
  P["host: own pick back -> replay stored"] = d(
    host.own_pick_back ?? host.tail_done,
    host.kf_done,
  );
  P["host: tail done -> replay stored"] = d(host.tail_done, host.kf_done);
  P["host: replay bytes"] = host.kf_bytes ?? join.offer_bytes ?? null;
  // Joiner: getting into the room.
  P["join: cmd -> Orca has it"] = d(join.cmd, join.cmd_rx);
  P["join: ticket mint (HTTP)"] = join.ticket_http_ms ?? null;
  P["join: cmd -> ticket asked"] = d(join.cmd_rx, join.ticket_req);
  P["join: ticket -> socket open"] = d(join.ticket_ok, join.ws_open);
  P["join: socket open -> welcome"] = d(join.ws_open, join.welcome);
  P["join: welcome -> host's hello"] = d(join.welcome, join.peer_here);
  P["join: cmd -> welcome (total)"] = d(join.cmd_rx, join.welcome);
  // The replay.
  P["replay stored (host) -> offer heard (join)"] = d(
    host.kf_done,
    join.offer_rx,
  );
  P["join: welcome -> offer heard (waits on host)"] = d(
    join.welcome,
    join.offer_rx,
  );
  P["join: download (offer -> 50%)"] = d(join.offer_rx, join.dl_done);
  P["join: offer -> rebuilt"] = d(join.offer_rx, join.rebuilt);
  P["join: rebuild (Orca's ms)"] = join.rebuild_ms ?? null;
  P["join: rebuilt -> plugged (catch-up + plug frame)"] = d(
    join.rebuilt,
    join.plugged,
  );
  P["join: catch-up (Orca's ms)"] = join.catchup_ms ?? null;
  P["join: plugged -> own pick in (steer)"] = friends
    ? null
    : d(join.plugged, join.p2_pick_in);
  // Direct link.
  P["direct link up after host welcome"] = d(
    host.welcome,
    host.dl_up ?? join.dl_up,
  );
  // What each player sees (Orca draws nothing while it restores, runs a tail, rebuilds or catches up).
  // The host's picture comes back at the tail's end, or with the unseen hold (newer builds) once
  // its own pick is back.
  const hostBack = host.shown ?? host.tail_done;
  P["SEE host: frozen picture (restore + unseen tail and hold)"] =
    hostBack && host.fresh_done
      ? Math.round(hostBack - host.fresh_done + (host.fresh_restore_ms ?? 0))
      : null;
  P["SEE host: room select, own hand steered (no control)"] = friends
    ? null
    : host.own_pick_back && hostBack
      ? Math.max(0, d(hostBack, host.own_pick_back))
      : null;
  P["SEE host: in control, waiting for the opponent's hand"] = friends
    ? null
    : d(host.own_pick_back, host.friend_joined);
  P["SEE host: opponent's hand steered in"] = friends
    ? null
    : d(host.friend_joined, host.p2_pick_in);
  P["SEE join: own select + 'Joining...' (until the replay is down)"] = d(
    join.matched,
    join.dl_done,
  );
  P["SEE join: frozen picture (restore + rebuild + catch-up)"] = d(
    join.dl_done,
    join.last_joining,
  );
  P["SEE join: room select drawn, not plugged yet"] = d(
    join.last_joining,
    join.plugged,
  );
  P["SEE join: own hand steered (no control)"] = friends
    ? null
    : d(join.plugged, join.p2_pick_in);
  // Totals from the match.
  P["TOTAL matched -> host restored (picture back)"] = d(t0, hostBack);
  P["TOTAL matched -> host's own pick back (host in control)"] = friends
    ? null
    : d(t0, host.own_pick_back);
  P["TOTAL matched -> replay stored"] = d(t0, host.kf_done);
  P["TOTAL matched -> joiner plugged (same game)"] = d(t0, join.plugged);
  P["TOTAL matched -> joiner's pick in (joiner in control)"] = friends
    ? null
    : d(t0, join.p2_pick_in);
  P["TOTAL matched -> both in control"] = friends
    ? d(t0, join.plugged)
    : join.p2_pick_in && host.own_pick_back
      ? d(
          t0,
          Math.max(join.p2_pick_in, host.own_pick_back, host.p2_pick_in ?? 0),
        )
      : null;
  P["TOTAL matched -> 'Press Start to lock in' (host)"] = friends
    ? null
    : d(t0, host.lockin);
  return P;
}

const pct = (xs, p) => {
  const s = xs
    .filter((x) => x != null && Number.isFinite(x))
    .sort((a, b) => a - b);
  if (!s.length) return null;
  const i = (s.length - 1) * p;
  const lo = Math.floor(i);
  const hi = Math.ceil(i);
  return Math.round(s[lo] + (s[hi] - s[lo]) * (i - lo));
};

// Load every side of every dir.
const sides = [];
for (const dir of dirs) {
  const metaPath = path.join(dir, "meta.json");
  const meta = fs.existsSync(metaPath)
    ? JSON.parse(fs.readFileSync(metaPath, "utf8"))
    : {};
  for (const f of fs.readdirSync(dir).filter((f) => f.endsWith(".raw.jsonl")))
    sides.push(loadSide(dir, f, meta));
}
// Two machines' clocks: every side on another machine than the first is moved onto the first's
// clock by the median gap between the two sides' `matched` over the rooms they share (both hear it
// from one queue socket at nearly the same moment; the median shrugs off a busy event loop).
{
  const machines = [...new Set(sides.map((s) => s.meta.host))];
  const ref = machines[0];
  for (const s of sides.filter((x) => x.meta.host !== ref)) {
    const mine = new Map(splitMatches(s).map((m) => [m.room, m.t0]));
    const gaps = [];
    for (const r of sides.filter((x) => x.meta.host === ref))
      for (const m of splitMatches(r))
        if (mine.has(m.room)) gaps.push(mine.get(m.room) - m.t0);
    gaps.sort((a, b) => a - b);
    const g = gaps.length ? gaps[Math.floor(gaps.length / 2)] : 0;
    s.clockShift = g;
    for (const row of s.rows) row.et -= g;
    if (DETAIL)
      console.error(
        `${s.name} (${s.meta.host}): clock moved by ${-g} ms (gaps ${gaps.join(" ")})`,
      );
  }
}
// Pair matches by room (and n for friends, whose room repeats).
const byKey = new Map();
for (const s of sides) {
  for (const m of splitMatches(s)) {
    const key = m.friends ? `${m.room}#${m.n}#${m.meta.start}` : m.room;
    if (!byKey.has(key)) byKey.set(key, []);
    byKey.get(key).push(m);
  }
}
const matches = [];
for (const [key, ms] of byKey) {
  if (ms.length !== 2) {
    if (DETAIL) console.error(`room ${key}: ${ms.length} side(s), skipped`);
    continue;
  }
  const ev = ms.map(sideEvents);
  const clockGap =
    ms[0].meta.host !== ms[1].meta.host
      ? (ev[1].matched ?? 0) - (ev[0].matched ?? 0)
      : 0;
  let hi = ev.findIndex((e, i) =>
    ms[i].friends ? ms[i].slot === 0 : e.role === "host",
  );
  if (hi < 0) hi = ms[0].slot === 0 ? 0 : 1;
  const host = ev[hi];
  const join = ev[1 - hi];
  const friends = ms[0].friends;
  const P = phases(host, join, friends);
  matches.push({
    key,
    clockGap,
    n: ms[hi].n,
    mode: ms[0].meta.mode,
    slug: ms[0].meta.slug,
    host: ms[hi].side,
    join: ms[1 - hi].side,
    platforms: [ms[hi].meta.platform, ms[1 - hi].meta.platform],
    P,
    hostEv: host,
    joinEv: join,
    ok:
      !host.error &&
      !join.error &&
      P["TOTAL matched -> both in control"] != null,
    failures: [...host.failures.map((f) => `host: ${f}`), ...join.failures.map((f) => `join: ${f}`)],
    // The replay's way over: the store's PUT (host) and GET (joiner), or inline in the offer.
    store_put: host.kf_inline === false ? 1 : 0,
    store_get: join.offer_inline === false ? 1 : 0,
    inline: join.offer_inline === true || host.kf_inline === true,
    dl: host.dl_kind ?? join.dl_kind,
    start: Math.min(host.matched, join.matched),
  });
}
matches.sort((a, b) => a.start - b.start);

const ok = matches.filter((m) => m.ok);
const label =
  LABEL || [...new Set(matches.map((m) => `${m.mode} ${m.slug}`))].join(", ");
console.log(
  `# ${label}: ${ok.length} complete matches of ${matches.length} paired (${dirs.length} run dir(s))`,
);
const names = ok.length ? Object.keys(ok[0].P) : [];
const rows = [];
for (const name of names) {
  const xs = ok.map((m) => m.P[name]).filter((x) => x != null);
  if (!xs.length) continue;
  rows.push({
    phase: name,
    n: xs.length,
    p50: pct(xs, 0.5),
    p90: pct(xs, 0.9),
    min: pct(xs, 0),
    max: pct(xs, 1),
  });
}
const w = Math.max(...rows.map((r) => r.phase.length), 10);
console.log(
  `${"phase".padEnd(w)}  ${"n".padStart(3)}  ${"p50".padStart(6)}  ${"p90".padStart(6)}  ${"min".padStart(6)}  ${"max".padStart(6)}  (ms)`,
);
for (const r of rows)
  console.log(
    `${r.phase.padEnd(w)}  ${String(r.n).padStart(3)}  ${String(r.p50).padStart(6)}  ${String(r.p90).padStart(6)}  ${String(r.min).padStart(6)}  ${String(r.max).padStart(6)}`,
  );
const kinds = {};
for (const m of ok) kinds[m.dl ?? "none"] = (kinds[m.dl ?? "none"] || 0) + 1;
{
  const sum = (k) => matches.reduce((a, m) => a + (m[k] ? Number(m[k]) : 0), 0);
  const sizes = matches.map((m) => m.P["host: replay bytes"]).filter((x) => x != null);
  console.log(
    `replay: ${sum("inline")} inline, store PUT ${sum("store_put")} GET ${sum("store_get")} over ${matches.length} matches; largest ${
      sizes.length ? Math.max(...sizes) : "?"
    } bytes`,
  );
}
const broken = matches.filter((m) => m.failures.length);
for (const m of broken)
  console.log(`BROKEN ${m.key} n=${m.n}: ${m.failures.join(" | ")}`);
console.log(
  `direct links: ${JSON.stringify(kinds)}; failed/partial: ${
    matches
      .filter((m) => !m.ok)
      .map(
        (m) => `${m.key}(${m.hostEv.error || m.joinEv.error || "incomplete"})`,
      )
      .join("; ") || "none"
  }`,
);
if (DETAIL) {
  for (const m of matches) {
    console.log(
      `\n## ${m.key} n=${m.n} host ${m.host} join ${m.join} ${m.ok ? "" : "INCOMPLETE"}`,
    );
    const t0 = m.start;
    const tl = [];
    for (const [who, e] of [
      ["host", m.hostEv],
      ["join", m.joinEv],
    ]) {
      for (const [k, v] of Object.entries(e))
        if (typeof v === "number" && v > 1e12) tl.push({ t: v - t0, who, k });
      for (const b of e.bars)
        tl.push({ t: b.t - t0, who, k: `bar '${b.bar}'` });
      for (const s of e.states) tl.push({ t: s.t - t0, who, k: s.s });
    }
    tl.sort((a, b) => a.t - b.t);
    for (const x of tl)
      if (x.t > -2000 && x.t < 15000)
        console.log(
          `  ${String(Math.round(x.t)).padStart(6)} ms  ${x.who.padEnd(4)}  ${x.k}`,
        );
  }
}
if (CSV_OUT) {
  const cols = ["key", "n", "mode", "slug", "host", "join", "dl", ...names];
  const lines = [cols.join(",")];
  for (const m of matches)
    lines.push(
      [
        m.key,
        m.n,
        m.mode,
        m.slug,
        m.host,
        m.join,
        m.dl,
        ...names.map((n) => m.P[n] ?? ""),
      ]
        .map((v) => JSON.stringify(v ?? ""))
        .join(","),
    );
  fs.writeFileSync(CSV_OUT, lines.join("\n") + "\n");
}
if (JSON_OUT)
  fs.writeFileSync(
    JSON_OUT,
    JSON.stringify(
      {
        label,
        rows,
        matches: matches.map((m) => ({
          key: m.key,
          n: m.n,
          ok: m.ok,
          host: m.host,
          join: m.join,
          dl: m.dl,
          inline: m.inline,
          store_put: m.store_put,
          store_get: m.store_get,
          failures: m.failures,
          P: m.P,
          hostBars: m.hostEv.bars.map((b) => ({
            t: b.t - m.start,
            bar: b.bar,
          })),
          joinBars: m.joinEv.bars.map((b) => ({
            t: b.t - m.start,
            bar: b.bar,
          })),
          hostStates: m.hostEv.states.map((s) => ({
            t: s.t - m.start,
            s: s.s,
          })),
          joinStates: m.joinEv.states.map((s) => ({
            t: s.t - m.start,
            s: s.s,
          })),
        })),
      },
      null,
      2,
    ),
  );
{
  const tot = rows.find((r) => r.phase === "TOTAL matched -> both in control");
  const bad = [];
  // A broken join fails the run whatever the times.
  if (broken.length) bad.push(`${broken.length} match(es) with a broken join (BROKEN lines above)`);
  if (ok.length < MIN_OK) bad.push(`${ok.length} complete matches < ${MIN_OK}`);
  if (!tot) bad.push("no complete match");
  else {
    if (MAX_P50 && tot.p50 > MAX_P50)
      bad.push(`p50 ${tot.p50} ms > ${MAX_P50}`);
    if (MAX_P90 && tot.p90 > MAX_P90)
      bad.push(`p90 ${tot.p90} ms > ${MAX_P90}`);
  }
  if (bad.length || MAX_P50 || MAX_P90 || MIN_OK)
    console.log(
      bad.length
        ? `FAIL: ${bad.join("; ")}`
        : `PASS: matched -> both in control p50 ${tot.p50} ms, p90 ${tot.p90} ms over ${ok.length} matches`,
    );
  process.exit(bad.length ? 1 : 0);
}
