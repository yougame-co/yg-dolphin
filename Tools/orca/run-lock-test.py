#!/usr/bin/env python3
# run-lock-test.py: unit tests for run-lock.py (the Mac's Orca run queue), in a scratch lock folder
# with a fake "Luis is playing" file and a fake load, so they never touch the real queue or Orca.
#
#   python3 Tools/orca/run-lock-test.py [-v] [Test.test_name]
#
# About 50 s. Each test starts real run-lock.py processes whose commands log when they start and end.
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "run-lock.py")
FLOCK = shutil.which("flock")
RECORD = (
    "import sys, time\n"
    "log, name, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])\n"
    "open(log, 'a').write(f'start {name} {time.time()}\\n')\n"
    "time.sleep(secs)\n"
    "open(log, 'a').write(f'end {name} {time.time()}\\n')\n"
)


def read(path):
    with open(path) as f:
        return f.read()


class RunLock(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.realpath(tempfile.mkdtemp(prefix="run-lock-test-"))
        self.log = os.path.join(self.dir, "events.log")
        self.playing = os.path.join(self.dir, "playing")
        self.env = dict(os.environ, ORCA_LOCK_DIR=self.dir, ORCA_RUN_LOCK_POLL="0.1",
                        ORCA_RUN_LOCK_TEST_PLAYING=self.playing,
                        ORCA_RUN_LOCK_TEST_LOAD=os.path.join(self.dir, "load"),
                        ORCA_RUN_LOCK_TEST_SSD=os.path.join(self.dir, "ssd"), ORCA_SSD_WATCH="",
                        ORCA_RUN_LOCK_TEST_GK=os.path.join(self.dir, "gk.log"), ORCA_GK_CACHE_S="0.2")
        for k in ("ORCA_RUN_LOCK", "QE2E_DEV_SUFFIX", "ORCA_CHECK_SLOTS", "ORCA_SSD_PROBE", "ORCA_SSD_GUARD",
                  "ORCA_SSD_STALL_S", "ORCA_SSD_QUIET_S", "ORCA_PERF_PPLUS_DIR", "ORCA_GK_GUARD", "ORCA_GK_BUSY_S",
                  "ORCA_GK_WAIT_MAX_S"):
            self.env.pop(k, None)
        self.set_load(3, 70)
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
                p.wait()
            for pipe in (p.stdout, p.stderr):
                if pipe:
                    pipe.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def set_load(self, load, free):
        with open(os.path.join(self.dir, "load"), "w") as f:
            f.write(f"{load} {free}\n")

    def ssd_stall(self, start, end, what="disk11 APFS tx_flush"):
        with open(os.path.join(self.dir, "ssd"), "a") as f:
            f.write(f"{start} {end} {what}\n")

    def gk(self, ago, what, path="9119bb72c798ec94", tid="419e553"):
        """One syspolicyd line, `ago` seconds old, as `log show --style compact` prints it."""
        t = time.time() - ago
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"
        msg = {"start": f"GK performScan: PST: (path: {path}), (team: (null)), (id: (null)), (bundle_id: (null))",
               "result": f"GK evaluateScanResult: 2, PST: (path: {path}), (team: (null)), (id: a.out), "
                         "(bundle_id: NOT_A_BUNDLE), 0, 0, 1, 0, 0, 0, 0",
               "failed": f"scan failed, finishing evaluation : PST: (path: {path}), (team: (null))"}[what]
        with open(os.path.join(self.dir, "gk.log"), "a") as f:
            f.write(f"{stamp} Df syspolicyd[503:{tid}] [com.apple.syspolicy.exec:default] {msg}\n")

    def record(self, name, secs):
        return [sys.executable, "-c", RECORD, self.log, name, str(secs)]

    def start(self, cls, cmd, *opts, env=None):
        p = subprocess.Popen([sys.executable, TOOL, cls, *opts, "--", *cmd], env=dict(self.env, **(env or {})),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(p)
        return p

    def finish(self, p, timeout=20):
        out, err = p.communicate(timeout=timeout)
        return p.returncode, out, err

    def events(self):
        if not os.path.exists(self.log):
            return []
        out = []
        for line in read(self.log).splitlines():
            kind, name, t = line.split()
            out.append((float(t), kind, name))
        return sorted(out)

    def spans(self):
        s = {}
        for t, kind, name in self.events():
            s.setdefault(name, [None, None])[0 if kind == "start" else 1] = t
        return s

    def most_at_once(self):
        n = best = 0
        for _, kind, _ in self.events():
            n += 1 if kind == "start" else -1
            best = max(best, n)
        return best

    def started(self, name):
        return any(kind == "start" and n == name for _, kind, n in self.events())

    def wait_for(self, cond, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if cond():
                return True
            time.sleep(0.05)
        return False

    def tickets(self):
        q = os.path.join(self.dir, "orca-run-queue")
        return sorted(n for n in os.listdir(q) if n.endswith(".ticket")) if os.path.isdir(q) else []

    # ---- tests ----

    def test_checks_share_the_machine(self):
        ps = [self.start("check", self.record(f"c{i}", 1.0)) for i in range(3)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 3)
        self.assertEqual(self.tickets(), [])

    def test_slot_cap(self):
        ps = [self.start("check", self.record(f"c{i}", 0.6), env={"ORCA_CHECK_SLOTS": "2"}) for i in range(5)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 2)
        self.assertEqual(len(self.spans()), 5)

    def test_busy_machine_runs_one_check_at_a_time(self):
        self.set_load(15, 70)
        ps = [self.start("check", self.record(f"c{i}", 0.5)) for i in range(3)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 1)
        self.set_load(5, 25)  # little memory free: one at a time too
        ps = [self.start("check", self.record(f"m{i}", 0.5)) for i in range(2)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 1)

    def test_overloaded_machine_runs_nothing(self):
        self.set_load(25, 70)
        rc, _, err = self.finish(self.start("check", self.record("c", 0.1), "--wait-max", "0.6"))
        self.assertEqual(rc, 9)
        self.assertFalse(self.started("c"))
        self.assertIn("allow 0", err)
        self.assertEqual(self.tickets(), [])

    def test_perf_waits_for_checks_and_checks_behind_it_wait(self):
        a = self.start("check", self.record("a", 1.5))
        self.assertTrue(self.wait_for(lambda: self.started("a")))
        perf = self.start("perf", self.record("p", 0.5))
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 2))
        b = self.start("check", self.record("b", 0.3))  # a slot is free, but the perf run came first
        for p in (a, perf, b):
            self.assertEqual(self.finish(p)[0], 0)
        s = self.spans()
        self.assertGreaterEqual(s["p"][0], s["a"][1])
        self.assertGreaterEqual(s["b"][0], s["p"][1])

    def test_check_waits_for_perf(self):
        perf = self.start("perf", self.record("p", 1.0))
        self.assertTrue(self.wait_for(lambda: self.started("p")))
        c = self.start("check", self.record("c", 0.2))
        rc, _, err = self.finish(c)
        self.assertEqual(rc, 0)
        self.assertEqual(self.finish(perf)[0], 0)
        s = self.spans()
        self.assertGreaterEqual(s["c"][0], s["p"][1])
        self.assertIn("a perf run has the machine", err)

    def test_nothing_starts_while_luis_plays(self):
        open(self.playing, "w").close()
        c = self.start("check", self.record("c", 0.2))
        perf = self.start("perf", self.record("p", 0.2))
        time.sleep(1.0)
        self.assertFalse(self.started("c") or self.started("p"))
        st = subprocess.run([sys.executable, TOOL, "status", "--luis"], env=self.env, capture_output=True, text=True)
        self.assertEqual(st.returncode, 0, st.stdout)
        self.assertIn("2 Orca run(s) waiting for Luis's Orca to close", st.stdout)
        os.unlink(self.playing)
        rc, _, err = self.finish(c)
        self.assertEqual(rc, 0)
        self.assertEqual(self.finish(perf)[0], 0)
        self.assertEqual(err.count("waiting: Luis's Orca is open"), 1, err)
        st = subprocess.run([sys.executable, TOOL, "status", "--luis"], env=self.env, capture_output=True, text=True)
        self.assertEqual(st.returncode, 1)

    def test_first_come_first_served(self):
        ps = []
        for i in range(4):
            ps.append(self.start("check", self.record(f"c{i}", 0.3), env={"ORCA_CHECK_SLOTS": "1"}))
            self.assertTrue(self.wait_for(lambda: len(self.tickets()) == i + 1 or self.started("c0")))
            time.sleep(0.15)
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        order = [name for _, kind, name in self.events() if kind == "start"]
        self.assertEqual(order, ["c0", "c1", "c2", "c3"])

    @unittest.skipUnless(FLOCK, "needs flock (brew install flock)")
    def test_plain_flock_holder_blocks_checks(self):
        solo = os.path.join(self.dir, "orca-run-solo.lock")
        legacy = subprocess.Popen([FLOCK, solo, *self.record("legacy", 1.0)], env=self.env)
        self.procs.append(legacy)
        self.assertTrue(self.wait_for(lambda: self.started("legacy")))
        rc, _, err = self.finish(self.start("check", self.record("c", 0.2)))
        self.assertEqual(rc, 0)
        legacy.wait()
        s = self.spans()
        self.assertGreaterEqual(s["c"][0], s["legacy"][1])
        self.assertIn("a run outside the queue holds orca-run-solo.lock", err)

    @unittest.skipUnless(FLOCK, "needs flock (brew install flock)")
    def test_plain_flock_waiter_goes_before_later_checks(self):
        solo = os.path.join(self.dir, "orca-run-solo.lock")
        two = os.path.join(self.dir, "orca-run.lock")
        a = self.start("check", self.record("a", 1.5))
        self.assertTrue(self.wait_for(lambda: self.started("a")))
        legacy = subprocess.Popen([FLOCK, solo, FLOCK, two, *self.record("legacy", 0.5)], env=self.env)
        self.procs.append(legacy)
        time.sleep(0.4)  # the plain flock is waiting for the check's shared lock now
        b = self.start("check", self.record("b", 0.2))
        self.assertEqual(self.finish(a)[0], 0)
        legacy.wait(timeout=20)
        rc, _, err = self.finish(b)
        self.assertEqual(rc, 0)
        s = self.spans()
        self.assertGreaterEqual(s["legacy"][0], s["a"][1])
        self.assertGreaterEqual(s["b"][0], s["legacy"][1])
        self.assertIn("plain flock run", err)

    def test_dead_runs_leave_no_ticket(self):
        open(self.playing, "w").close()
        dead = self.start("check", self.record("dead", 0.1))
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 1))
        dead.kill()
        dead.wait()
        self.assertEqual(len(self.tickets()), 1)  # left behind, but nobody holds it
        os.unlink(self.playing)
        rc, _, err = self.finish(self.start("check", self.record("c", 0.1)))
        self.assertEqual(rc, 0)
        self.assertNotIn("ahead in the queue", err)
        self.assertEqual(self.tickets(), [])

    def test_gives_up_after_wait_max(self):
        open(self.playing, "w").close()
        t0 = time.time()
        rc, _, err = self.finish(self.start("perf", self.record("p", 0.1), "--wait-max", "0.5"))
        self.assertEqual(rc, 9)
        self.assertLess(time.time() - t0, 5)
        self.assertIn("gave up waiting", err)
        self.assertEqual(self.tickets(), [])

    def test_exit_code_passes_through(self):
        rc, _, err = self.finish(self.start("check", ["sh", "-c", "exit 3"]))
        self.assertEqual(rc, 3)
        self.assertIn("done: exit 3", err)

    def test_nested_runs(self):
        inner = [sys.executable, TOOL, "check", "--", *self.record("inner", 0.1)]
        rc, _, err = self.finish(self.start("check", inner))
        self.assertEqual(rc, 0)
        self.assertIn("already inside a check run", err)
        rc, _, err = self.finish(self.start("perf", inner))
        self.assertEqual(rc, 0)
        rc, _, err = self.finish(self.start("check", [sys.executable, TOOL, "perf", "--", "true"]))
        self.assertEqual(rc, 2)
        self.assertIn("cannot start inside a check run", err)

    @unittest.skipUnless(FLOCK, "needs flock (brew install flock)")
    def test_command_taking_the_old_lock_is_stopped(self):
        solo = os.path.join(self.dir, "orca-run-solo.lock")
        t0 = time.time()
        rc, _, err = self.finish(self.start("check", [FLOCK, solo, "sleep", "30"]))
        self.assertEqual(rc, 11)
        self.assertLess(time.time() - t0, 10)
        self.assertIn("takes orca-run-solo.lock itself", err)
        # A shared flock inside a check is fine; inside a perf run it is not.
        rc, _, _ = self.finish(self.start("check", [FLOCK, "-s", solo, "true"]))
        self.assertEqual(rc, 0)
        rc, _, _ = self.finish(self.start("perf", [FLOCK, "-s", solo, "sleep", "30"]))
        self.assertEqual(rc, 11)

    def test_signal_stops_the_whole_command(self):
        pidfile = os.path.join(self.dir, "grandchild")
        c = self.start("check", ["sh", "-c", f"sleep 60 & echo $! > {pidfile}; wait"])
        self.assertTrue(self.wait_for(lambda: os.path.exists(pidfile) and read(pidfile).strip()))
        grandchild = int(read(pidfile))
        c.send_signal(signal.SIGTERM)
        rc, _, err = self.finish(c)
        self.assertEqual(rc, 128 + signal.SIGTERM)
        time.sleep(0.2)
        with self.assertRaises(ProcessLookupError):
            os.kill(grandchild, 0)
        self.assertEqual(self.tickets(), [])

    def test_a_check_stops_when_luis_opens_his_orca(self):
        c = self.start("check", ["sleep", "30"])  # no option: stopping is a check's default
        self.assertTrue(self.wait_for(lambda: any(".running." in t for t in self.tickets())))
        open(self.playing, "w").close()
        t0 = time.time()
        rc, _, err = self.finish(c)
        self.assertEqual(rc, 10)
        self.assertLess(time.time() - t0, 5)
        self.assertIn("stopped: Luis's Orca is open", err)
        self.assertEqual(self.tickets(), [])
        os.unlink(self.playing)
        c = self.start("check", ["sleep", "30"], "--yield")  # still accepted, same thing
        self.assertTrue(self.wait_for(lambda: any(".running." in t for t in self.tickets())))
        open(self.playing, "w").close()
        self.assertEqual(self.finish(c)[0], 10)

    def test_no_yield_check_runs_on_and_notes_it(self):
        c = self.start("check", self.record("c", 1.0), "--no-yield")
        self.assertTrue(self.wait_for(lambda: self.started("c")))
        open(self.playing, "w").close()
        rc, _, err = self.finish(c)
        self.assertEqual(rc, 0)
        self.assertIn("it runs on beside his game", err)
        self.assertEqual(err.count("Luis's Orca opened"), 1, err)
        self.assertIsNotNone(self.spans()["c"][1])
        rc, _, err = self.finish(self.start("check", ["true"], "--yield", "--no-yield"))
        self.assertEqual(rc, 2)
        self.assertIn("not allowed with argument", err)

    def test_perf_notes_luis_opening_his_orca(self):
        perf = self.start("perf", self.record("p", 1.0))
        self.assertTrue(self.wait_for(lambda: self.started("p")))
        open(self.playing, "w").close()
        rc, _, err = self.finish(perf)
        self.assertEqual(rc, 0)
        self.assertIn("numbers from here on are suspect", err)
        self.assertEqual(err.count("Luis's Orca opened"), 1, err)
        os.unlink(self.playing)
        perf = self.start("perf", ["sleep", "30"], "--yield")  # a perf run stops only when asked to
        self.assertTrue(self.wait_for(lambda: any(".running." in t for t in self.tickets())))
        open(self.playing, "w").close()
        rc, _, err = self.finish(perf)
        self.assertEqual(rc, 10)
        self.assertIn("stopped: Luis's Orca is open", err)

    def test_shared_dev_game_queue_e2e_runs_one_at_a_time(self):
        old = os.path.join(self.dir, "old", "queue-e2e.mjs")
        new = os.path.join(self.dir, "new", "queue-e2e.mjs")
        for path, text in ((old, "const DEV_GAME = `orca-queue-e2e-${PROFILE}`;\n"),
                           (new, "process.env.QE2E_DEV_SUFFIX\n")):
            os.makedirs(os.path.dirname(path))
            with open(path, "w") as f:
                f.write(text)
        ps = [self.start("check", self.record(f"old{i}", 0.6) + [old]) for i in range(2)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 1)
        os.unlink(self.log)
        ps = [self.start("check", self.record(f"new{i}", 0.6) + [new]) for i in range(2)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 2)
        os.unlink(self.log)  # --slug: two signed-in players, one at a time whatever the file
        ps = [self.start("check", self.record(f"slug{i}", 0.6) + [new, "--slug", "x"]) for i in range(2)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 1)

    def test_perf_waits_behind_a_running_perf_and_says_so(self):
        first = self.start("perf", self.record("p1", 1.0))
        self.assertTrue(self.wait_for(lambda: self.started("p1")))
        rc, _, err = self.finish(self.start("perf", self.record("p2", 0.1)))
        self.assertEqual(rc, 0)
        self.assertEqual(self.finish(first)[0], 0)
        self.assertIn("a perf run has the machine", err)
        s = self.spans()
        self.assertGreaterEqual(s["p2"][0], s["p1"][1])

    def test_a_check_passes_one_that_waits_for_its_serial_key(self):
        a = self.start("check", self.record("a", 1.5), "--serial", "k")
        self.assertTrue(self.wait_for(lambda: self.started("a")))
        b = self.start("check", self.record("b", 0.2), "--serial", "k")
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 2))
        time.sleep(0.3)
        c = self.start("check", self.record("c", 0.2))
        for p in (a, b, c):
            self.assertEqual(self.finish(p)[0], 0)
        s = self.spans()
        self.assertLess(s["c"][1], s["a"][1])  # c went past b, which waited for a's key
        self.assertGreaterEqual(s["b"][0], s["a"][1])

    def test_runs_sharing_a_chosen_dev_suffix_are_kept_apart(self):
        new = os.path.join(self.dir, "queue-e2e.mjs")
        with open(new, "w") as f:
            f.write("process.env.QE2E_DEV_SUFFIX\n")
        env = {"QE2E_DEV_SUFFIX": "Mine"}
        ps = [self.start("check", self.record(f"s{i}", 0.6) + [new], env=env) for i in range(2)]
        for p in ps:
            self.assertEqual(self.finish(p)[0], 0)
        self.assertEqual(self.most_at_once(), 1)

    def test_signal_while_waiting(self):
        open(self.playing, "w").close()
        c = self.start("check", self.record("c", 0.1))
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 1))
        time.sleep(0.3)
        c.send_signal(signal.SIGTERM)
        self.assertEqual(self.finish(c)[0], 128 + signal.SIGTERM)
        self.assertEqual(self.tickets(), [])
        # Started under nohup (SIGHUP ignored), a hangup changes nothing.
        n = subprocess.Popen(["nohup", sys.executable, TOOL, "check", "--", *self.record("n", 0.1)],
                             env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.procs.append(n)
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 1))
        time.sleep(0.3)
        n.send_signal(signal.SIGHUP)
        time.sleep(0.5)
        self.assertIsNone(n.poll())
        os.unlink(self.playing)
        self.assertEqual(n.wait(timeout=10), 0)
        self.assertTrue(self.started("n"))

    def test_status(self):
        c = self.start("check", self.record("c", 1.5), "--label", "the check")
        self.assertTrue(self.wait_for(lambda: self.started("c")))
        perf = self.start("perf", self.record("p", 0.1), "--label", "the perf run")
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 2))
        time.sleep(0.3)
        st = subprocess.run([sys.executable, TOOL, "status", "--json"], env=self.env, capture_output=True, text=True)
        data = json.loads(st.stdout)
        self.assertFalse(data["luis_orca_open"])
        got = [(t["state"], t["cls"], t["label"]) for t in data["tickets"]]
        self.assertEqual(got, [("running", "check", "the check"), ("waiting", "perf", "the perf run")])
        self.assertIn("still going", data["tickets"][1]["reason"])
        text = subprocess.run([sys.executable, TOOL, "status"], env=self.env, capture_output=True, text=True).stdout
        self.assertIn("Luis's Orca: closed", text)
        self.assertIn("the perf run", text)
        for p in (c, perf):
            self.assertEqual(self.finish(p)[0], 0)

    # ---- macOS's check of new programs ----

    def test_gk_pending_reads_the_log(self):
        spec = importlib.util.spec_from_file_location("run_lock", TOOL)
        rl = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rl)
        self.gk(130, "start", "aaaa")                   # next-swc: 125 s, then its result
        self.gk(5, "result", "aaaa", tid="41a0975")
        self.gk(5, "start", "bbbb")                     # the next on the same thread, still going
        self.gk(60, "start", "cccc", tid="41a0001")     # another thread: ended by "scan failed"
        self.gk(59, "failed", "cccc", tid="41a0002")
        self.gk(50, "start", "dddd", tid="41b0000")     # one whose end was never logged...
        self.gk(40, "start", "eeee", tid="41b0000")     # ...is over once its thread scans the next
        self.gk(39, "result", "eeee", tid="41c0000")
        lines = read(os.path.join(self.dir, "gk.log")).splitlines() + ["Timestamp  Ty Process[PID:TID]", "garbage"]
        self.assertEqual([p for _, p in rl.gk_pending(lines)], ["bbbb"])
        self.assertAlmostEqual(rl.gk_pending(lines)[0][0], time.time() - 5, delta=1.5)

    def test_runs_wait_while_macos_checks_a_program(self):
        self.gk(30, "start")
        c = self.start("check", self.record("c", 0.1))
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 1))
        time.sleep(0.5)
        self.assertFalse(self.started("c"))
        text = subprocess.run([sys.executable, TOOL, "status"], env=self.env, capture_output=True, text=True).stdout
        self.assertIn("macOS: a new-program check running 3", text)
        self.gk(0, "result")
        rc, _, err = self.finish(c)
        self.assertEqual(rc, 0)
        self.assertIn("waiting: macOS is still checking a new program (3", err)
        text = subprocess.run([sys.executable, TOOL, "status"], env=self.env, capture_output=True, text=True).stdout
        self.assertIn("macOS: no new-program check under way", text)
        # A check that has run under ORCA_GK_BUSY_S is a normal one: nobody waits.
        self.gk(1, "start", "ffff")
        rc, _, err = self.finish(self.start("check", self.record("d", 0.1)))
        self.assertEqual(rc, 0)
        self.assertNotIn("macOS", err)

    def test_a_check_that_never_ends_is_waited_for_only_so_long(self):
        self.gk(600, "start")
        rc, _, err = self.finish(self.start("perf", self.record("p", 0.1), env={"ORCA_GK_WAIT_MAX_S": "0.5"}))
        self.assertEqual(rc, 0)
        self.assertIn("waiting: macOS is still checking a new program (10m", err)
        self.assertIn("note: macOS has been checking a new program for 10m", err)
        # ORCA_GK_GUARD=0: no look at all.
        rc, _, err = self.finish(self.start("check", self.record("q", 0.1), env={"ORCA_GK_GUARD": "0"}))
        self.assertEqual(rc, 0)
        self.assertNotIn("macOS", err)

    # ---- the external SSD (perf runs) ----

    def test_perf_waits_for_a_quiet_ssd(self):
        now = time.time()
        self.ssd_stall(now - 3, now)
        perf = self.start("perf", self.record("p", 0.1), "--label", "timed", env={"ORCA_SSD_QUIET_S": "1.5"})
        self.assertTrue(self.wait_for(lambda: len(self.tickets()) == 1))
        time.sleep(0.4)
        self.assertFalse(self.started("p"))
        text = subprocess.run([sys.executable, TOOL, "status"], env=self.env, capture_output=True, text=True).stdout
        self.assertIn("the external SSD stalled 3 s at", text)
        self.assertIn("keep the external SSD quiet", text)
        rc, _, err = self.finish(perf)
        self.assertEqual(rc, 0)
        self.assertGreaterEqual(self.spans()["p"][0], now + 1.5)
        self.assertIn("waiting: the external SSD stalled 3 s", err)
        self.assertIn("the external SSD quiet for 2 s", err)  # 1.5 rounds to 2 in the start line
        self.assertIn("SSD: no stall over 2 s during this perf run", err)

    def test_checks_do_not_wait_for_the_ssd(self):
        now = time.time()
        self.ssd_stall(now - 30, now)
        rc, _, err = self.finish(self.start("check", self.record("c", 0.1)))
        self.assertEqual(rc, 0)
        self.assertNotIn("SSD", err)
        self.assertNotIn("keep the external SSD quiet",
                         subprocess.run([sys.executable, TOOL, "status"], env=self.env, capture_output=True,
                                        text=True).stdout)

    def test_perf_reports_stalls_during_the_run(self):
        ssd = os.path.join(self.dir, "ssd")
        stall = ("import sys, time\n"
                 "time.sleep(0.3); t = time.time()\n"
                 "open(sys.argv[1], 'a').write(f'{t - 2.5} {t} disk11 APFS tx_flush\\n')\n"
                 "time.sleep(0.3)\n")
        rc, _, err = self.finish(self.start("perf", [sys.executable, "-c", stall, ssd]))
        self.assertEqual(rc, 0)
        self.assertIn("SSD: TAINTED: the external SSD stalled 1 time(s) during this perf run", err)
        self.assertIn("2.5 s (disk11 APFS tx_flush)", err)
        # A stall that was over before the run started taints nothing; the next run waits it out.
        rc, _, err = self.finish(self.start("perf", self.record("p", 0.1), env={"ORCA_SSD_QUIET_S": "0.5"}))
        self.assertEqual(rc, 0)
        self.assertIn("SSD: no stall", err)

    def test_a_read_that_hangs_holds_perf_back(self):
        fifo = os.path.join(self.dir, "drive")
        os.mkfifo(fifo)  # opening it for reading blocks until a writer comes: a read that hangs
        env = {"ORCA_SSD_PROBE": fifo, "ORCA_SSD_STALL_S": "0.3", "ORCA_SSD_QUIET_S": "0.5"}
        perf = self.start("perf", self.record("p", 0.1), env=env)

        def reason():
            data = json.loads(subprocess.run([sys.executable, TOOL, "status", "--json"], env=self.env,
                                             capture_output=True, text=True).stdout)
            return data["tickets"][0].get("reason", "") if data["tickets"] else ""

        self.assertTrue(self.wait_for(lambda: "is stalling now" in reason()))
        time.sleep(0.5)
        self.assertFalse(self.started("p"))
        regular = os.path.join(self.dir, "drive.new")
        with open(regular, "wb") as f:
            f.write(b"x" * 4096)
        w = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)  # lets the hung open through
        os.replace(regular, fifo)  # later reads find a plain file
        os.close(w)  # and the hung read ends
        rc, _, err = self.finish(perf)
        self.assertEqual(rc, 0)
        self.assertIn("waiting: the external SSD is stalling now: a 1 MB read of drive", err)
        self.assertIn("a 1 MB read of drive took", err)  # what it waited for once the read came back
        self.assertIn("SSD: no stall", err)

    def test_pplus_comes_from_the_internal_copy(self):
        spec = importlib.util.spec_from_file_location("run_lock", TOOL)
        rl = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rl)
        ext, mine = os.path.join(self.dir, "ssd-pplus"), os.path.join(self.dir, "internal-pplus")
        dol = "Project+ Netplay Launcher.dol"
        for d in (ext, mine):
            os.makedirs(d)
            with open(os.path.join(d, dol), "wb") as f:
                f.write(b"launcher")
            with open(os.path.join(d, "sd.raw"), "wb") as f:
                f.write(b"\0" * 1000)
            os.utime(os.path.join(d, "sd.raw"), (1e9, 1e9))
        rl.PPLUS_INTERNAL = mine
        rl.on_external_disk = lambda path, _internal: path.startswith(ext)
        cmd = ["node", "queue-e2e.mjs", "bin", "disc", "--pplus", os.path.join(ext, dol)]
        out, swapped = rl.internal_pplus(cmd)
        self.assertEqual(out, cmd[:-1] + [os.path.join(mine, dol)])
        self.assertEqual(swapped, [(os.path.join(ext, dol), os.path.join(mine, dol))])
        out, _ = rl.internal_pplus(["x", "--pplus=" + os.path.join(ext, dol)])
        self.assertEqual(out, ["x", "--pplus=" + os.path.join(mine, dol)])
        # Another release (a different SD image) keeps the path as given.
        with open(os.path.join(mine, "sd.raw"), "ab") as f:
            f.write(b"\0")
        self.assertEqual(rl.internal_pplus(cmd), (cmd, []))
        rl.PPLUS_INTERNAL = ""
        self.assertEqual(rl.internal_pplus(cmd), (cmd, []))

    def test_probe_file_is_made_once(self):
        spec = importlib.util.spec_from_file_location("run_lock", TOOL)
        rl = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rl)
        path = rl.probe_file(self.dir)
        self.assertEqual(path, os.path.join(self.dir, ".orca-ssd-probe"))
        self.assertEqual(os.path.getsize(path), 64 << 20)
        made = os.stat(path).st_mtime_ns
        self.assertEqual(rl.probe_file(self.dir), path)
        self.assertEqual(os.stat(path).st_mtime_ns, made)
        rl.read_probe(path)
        self.assertEqual(sorted(os.listdir(self.dir)), [".orca-ssd-probe", "load"])


if __name__ == "__main__":
    unittest.main()
