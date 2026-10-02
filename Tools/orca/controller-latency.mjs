#!/usr/bin/env node
// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later
//
// How much latency the YouGame app's controller stream adds between a GameCube adapter's USB report
// and Orca having it as its local pad, measured through the real desktop code: this script runs the
// app's ControllerService and Bridge (desktop/src/controllers.ts, bridge.ts) with a scripted helper
// in place of the native one, and Orca's own stream client (Core/Orca/UX/ControllerSource) reads it
// from the unit test binary. The helper stamps each report's receivedAt the way the real helper
// does (an epoch-anchored monotonic clock) and follows its one-in-flight ack rule; Orca records
// arrival - receivedAt per report. What Dolphin's own GCAdapter adds over the same USB read is a
// mutex and a struct copy, so this number is the difference between the two paths.
//
//   # once: the desktop sources as plain JS (any of these works)
//   npm --prefix ~/Documents/YouGame/desktop run build            # -> desktop/dist
//   #   or: esbuild desktop/src/{bridge,controllers}.ts --outdir=/tmp/ygd --format=cjs --platform=node
//   YOUGAME_DESKTOP_DIST=~/Documents/YouGame/desktop/dist RATE_HZ=1000 SECONDS=20 \
//     node Tools/orca/controller-latency.mjs build/Binaries/Tests/tests
//
// RATE_HZ is the adapter's report rate (125 stock, 1000 overclocked). Prints Orca's percentiles.
import { spawn } from "node:child_process";
import { createRequire } from "node:module";
import { performance } from "node:perf_hooks";
import path from "node:path";
import readline from "node:readline";
import { fileURLToPath } from "node:url";

const epochNow = () => performance.timeOrigin + performance.now();

if (process.env.ORCA_FAKE_HELPER === "1") {
  // The scripted helper: the native helper's stdout protocol (desktop/native/src/main.cpp).
  const session = process.argv[2];
  const rate = Number(process.env.RATE_HZ || 1000);
  let sequence = 0,
    inFlight = false,
    dirty = false,
    receivedAt = 0,
    report = 0;
  const port = (i, buttons) => ({
    adapterId: `${session}-gc`,
    port: i,
    seat: i,
    connected: i === 0,
    type: i === 0 ? "wired" : null,
    buttons: i === 0 ? buttons : 0,
    axes: [128, 128, 128, 128],
    triggers: [0, 0],
    origin: [128, 128, 128, 128, 0, 0],
    calibrationRevision: 0,
  });
  const send = () => {
    dirty = false;
    inFlight = true;
    const snapshot = {
      schema: 1,
      session,
      sequence: ++sequence,
      sentAt: epochNow(),
      receivedAt,
      source: "native",
      state: "connected",
      message: "",
      owned: true,
      suspended: false,
      invalidReports: 0,
      ports: [0, 1, 2, 3].map((i) => port(i, report & 1 ? 1 : 0)),
      pads: [],
    };
    process.stdout.write(JSON.stringify({ type: "snapshot", snapshot }) + "\n");
  };
  // A USB report: as the real helper, it marks the state dirty and publishes unless a snapshot is
  // still waiting for its ack (then the next ack publishes the newest).
  setInterval(() => {
    receivedAt = epochNow();
    ++report;
    dirty = true;
    if (!inFlight) send();
  }, 1000 / rate);
  readline.createInterface({ input: process.stdin }).on("line", (line) => {
    const [id, action] = line.split(" ");
    if (action === "ack") {
      if (Number(id) === sequence) {
        inFlight = false;
        if (dirty) send();
      }
    } else if (action === "quit") process.exit(0);
    else
      process.stdout.write(
        JSON.stringify({ type: "reply", id, ok: true, result: null }) + "\n",
      );
  });
} else {
  const tests = process.argv[2];
  const dist = process.env.YOUGAME_DESKTOP_DIST;
  if (!tests || !dist) {
    console.error(
      "usage: YOUGAME_DESKTOP_DIST=<desktop dist> node controller-latency.mjs <tests binary>",
    );
    process.exit(2);
  }
  const require = createRequire(import.meta.url);
  const { ControllerService } = require(path.resolve(dist, "controllers.js"));
  const { Bridge } = require(path.resolve(dist, "bridge.js"));
  process.env.ORCA_FAKE_HELPER = "1";
  const controllers = new ControllerService(fileURLToPath(import.meta.url));
  const bridge = new Bridge(
    "orca-latency",
    async () => ({ ok: true, result: null }),
    controllers,
  );
  const url = await bridge.start();
  const child = spawn(tests, ["--gtest_filter=OrcaUXLatency.BridgeTransport"], {
    stdio: ["ignore", "pipe", "inherit"],
    env: {
      ...process.env,
      ORCA_FAKE_HELPER: "",
      ORCA_UX_LATENCY_BRIDGE: url,
      ORCA_UX_LATENCY_TOKEN: bridge.token,
      ORCA_UX_LATENCY_SECONDS: process.env.SECONDS || "10",
    },
  });
  let out = "";
  child.stdout.on("data", (d) => (out += d));
  const code = await new Promise((r) => child.once("exit", r));
  const line = out.split("\n").find((l) => l.startsWith("orca ux latency"));
  console.log(line || out);
  bridge.close();
  controllers.close();
  process.exit(code ?? 1);
}
