#!/usr/bin/env node
// match-latency-session.mjs <YouGame checkout> <out dir> <roster name> [<roster name> ...]
// Session Cookie headers for /sandbox roster players (sandbox-<name>), minted server-side by the
// YouGame repo's online-tests helper (web/scripts/online-tests/lib.mjs roster(): never a magic link
// in a browser; needs SUPABASE_ACCESS_TOKEN), written as mode-600 files <out>/<name>.cookie, plus
// <out>/ids.json with their player ids, for match-latency.mjs --sess. Only roster accounts: never a
// real player's. Prints names and ids, never a cookie. Delete the folder when done.
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const [repo, out, ...names] = process.argv.slice(2);
if (!repo || !out || !names.length) {
  console.error("usage: match-latency-session.mjs <YouGame checkout> <out dir> <roster name>...");
  process.exit(2);
}
const { roster } = await import(
  pathToFileURL(path.join(repo, "web/scripts/online-tests/lib.mjs")).href
);
fs.mkdirSync(out, { recursive: true, mode: 0o700 });
const idsFile = path.join(out, "ids.json");
const ids = fs.existsSync(idsFile) ? JSON.parse(fs.readFileSync(idsFile, "utf8")) : {};
for (const n of names) {
  const r = await roster(n);
  const file = path.join(out, `${n}.cookie`);
  fs.writeFileSync(file, r.cookie, { mode: 0o600 });
  fs.chmodSync(file, 0o600);
  ids[n] = { id: r.id, username: r.username, user: r.user, name: r.name };
  console.log(`${r.username}: cookie written (${file}), player id ${r.id}`);
}
fs.writeFileSync(idsFile, JSON.stringify(ids, null, 2));
process.exit(0);
