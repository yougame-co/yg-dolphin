// The first seconds of each match in a windowed harness run (ORCA.md, "Match start"): the host's
// VI intervals (Graphics.Settings.LogRenderTimeToFile writes them to <user>/Logs/vblank_times.txt)
// from 0.6 s before each scMelee scene to 12 s after it. Works on any Orca build, released ones too:
// it needs only the log and the VI file.
//
//   node Tools/orca/match-start.mjs <run folder> [...]
//
// A run folder holds:
//   stdout.log         the run's whole output, with YG_SCENES=1 (its "Scene frame" lines) and
//                      Logger.Logs.ROLLBACK=True; the log's own MM:SS:mmm stamps place the scenes
//   vblank_times.txt   copied from <user>/Logs after the run (delete it before the run)
//   meta.txt           "t_start <unix seconds>" and "t_end <unix seconds>" around the process
// The VI series is placed in time by its end: the "Diag frame times" line if the run has one
// (YG_FRAMETIME, printed right after the last frame), else t_end.
import fs from "node:fs";
import path from "node:path";

const VI_MS = 1000 / 59.94;
const BEFORE_S = 0.6;
const AFTER_S = 12;

// Windows PowerShell's `*>` writes UTF-16 with a byte order mark; everything else here is 8-bit.
function readText(file) {
  const bytes = fs.readFileSync(file);
  if (bytes[0] === 0xff && bytes[1] === 0xfe) return bytes.subarray(2).toString("utf16le");
  if (bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf)
    return bytes.subarray(3).toString("utf8");
  return bytes.toString("latin1");
}

function readMeta(dir) {
  const meta = {};
  for (const line of readText(path.join(dir, "meta.txt")).split(/\r?\n/)) {
    const m = /^(\S+)\s+(.*)$/.exec(line.trim());
    if (m) meta[m[1]] = m[2];
  }
  return meta;
}

// "MM:SS:mmm" (the wall clock's minutes and seconds) to unix seconds near tStart.
function logTime(stamp, tStart) {
  const [m, s, ms] = stamp.split(":").map(Number);
  let t = Math.floor(tStart / 3600) * 3600 + m * 60 + s + ms / 1000;
  while (t < tStart - 5) t += 3600;
  return t;
}

function stats(v) {
  const over = (x) => v.filter((y) => y > x).length;
  return {
    n: v.length,
    max: v.length ? Math.round(Math.max(...v) * 10) / 10 : 0,
    over25: over(25),
    over33: over(33.4),
    over50: over(50),
    lostMs: Math.round(
      v.filter((x) => x > 20).reduce((a, x) => a + x - VI_MS, 0),
    ),
  };
}

function run(dir) {
  const meta = readMeta(dir);
  const t0 = Number(meta.t_start);
  const scenes = [];
  let diagT = null;
  let boot = null;
  for (const line of readText(path.join(dir, "stdout.log")).split(/\r?\n/)) {
    let m = /^(\d+:\d+:\d+) .*Scene frame (\d+): (\S*) -> (\S+)/.exec(line);
    if (m) scenes.push({ t: logTime(m[1], t0), frame: Number(m[2]), to: m[4] });
    m = /^(\d+:\d+:\d+) .*Diag frame times/.exec(line);
    if (m) diagT = logTime(m[1], t0);
    m = /Orca: (compiled .*|stopped waiting .*)$/.exec(line);
    if (m) boot = m[1];
  }
  const vbPath = path.join(dir, "vblank_times.txt");
  const vb = fs.existsSync(vbPath)
    ? readText(vbPath)
        .split(/\r?\n/)
        .filter((x) => x.trim())
        .map(Number)
    : [];
  const anchor = diagT ?? Number(meta.t_end);
  const total = vb.reduce((a, x) => a + x, 0);
  let acc = 0;
  const vis = vb.map((x) => {
    acc += x;
    return { ms: x, end: anchor - (total - acc) / 1000 };
  });
  const out = [`== ${path.basename(dir)}${boot ? `  (shaders: ${boot})` : ""}`];
  if (!vb.length) out.push("  no vblank_times.txt");
  scenes
    .filter((s) => s.to === "scMelee")
    .forEach((s, i) => {
      const inWindow = vis.filter(
        (v) => v.end >= s.t - BEFORE_S && v.end < s.t + AFTER_S,
      );
      const st = stats(inWindow.map((v) => v.ms));
      const stalls = inWindow
        .filter((v) => v.ms > 33.4)
        .map(
          (v) =>
            `${(v.end - v.ms / 1000 - s.t).toFixed(2)}s:${Math.round(v.ms)}`,
        );
      out.push(
        `  match ${i + 1} (frame ${s.frame}): VI n=${st.n} >25 ms ${st.over25}, >33 ms ${st.over33}, ` +
          `>50 ms ${st.over50}, worst ${st.max} ms, lost ${st.lostMs} ms`,
      );
      out.push(
        `    VI over 33 ms (seconds after scMelee:ms): ${stalls.join(" ") || "none"}`,
      );
    });
  return out.join("\n");
}

const dirs = process.argv.slice(2);
if (!dirs.length) {
  console.error("usage: node Tools/orca/match-start.mjs <run folder> [...]");
  process.exit(2);
}
for (const d of dirs) console.log(run(d));
