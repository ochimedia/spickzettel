# Commit latency by journal setting, on an idle and a busy disk - see
# docs/PERF.md, "Results: the journal, on a busy disk".
#
# Each setting has its own database. The settings take turns in rounds, so
# a busy phase of the disk hits all of them alike. A commit is what the app
# makes for a stroke: one snippet row upserted (a 560-byte record and a
# 2 KB stroke blob) in its own transaction, one every 50 ms. After each
# round, a WAL database is checkpointed and that is timed too - the flush
# WAL puts off, paid when the app chooses (when the overlay hides).
#
#   python journal_bench.py <condition> <rounds> <per-round>
#   (docs/PERF.md ran idle, moderate and heavy, each 6 20)
#   condition: idle | moderate | heavy  (moderate/heavy start hog.py)
import json, os, sqlite3, statistics, subprocess, sys, time

condition = sys.argv[1]
rounds = int(sys.argv[2])
per_round = int(sys.argv[3])
here = os.path.dirname(os.path.abspath(__file__))
base = os.path.join(os.environ["LOCALAPPDATA"], "Temp", "sz-journal-bench")
os.makedirs(base, exist_ok=True)

settings = [
    ("rollback FULL (today)", "DELETE", "FULL", False),
    ("WAL FULL", "WAL", "FULL", False),
    ("WAL FULL excl", "WAL", "FULL", True),
    ("WAL NORMAL", "WAL", "NORMAL", False),
    ("WAL NORMAL excl", "WAL", "NORMAL", True),
]

conns = {}
for name, journal, sync, excl in settings:
    path = os.path.join(base, name.replace(" ", "_").replace("(", "").replace(")", "") + ".db")
    for suffix in ("", "-journal", "-wal", "-shm"):
        try:
            os.remove(path + suffix)
        except FileNotFoundError:
            pass
    c = sqlite3.connect(path, isolation_level=None, timeout=5)
    if excl:
        c.execute("PRAGMA locking_mode = EXCLUSIVE")
    c.execute("PRAGMA auto_vacuum = INCREMENTAL")
    assert c.execute(f"PRAGMA journal_mode = {journal}").fetchone()[0].upper() == journal
    c.execute(f"PRAGMA synchronous = {sync}")
    c.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, record TEXT, strokes BLOB)")
    for i in range(30):
        c.execute("INSERT INTO items VALUES (?, ?, ?)", (i, "x" * 560, os.urandom(2000)))
    if journal == "WAL":
        c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    conns[name] = (c, journal)

hog = None
if condition != "idle":
    hog = subprocess.Popen([sys.executable, os.path.join(here, "hog.py"), condition, base])
    time.sleep(3)

commits = {name: [] for name, *_ in settings}
checkpoints = {name: [] for name, j, _, _ in settings if j == "WAL"}
try:
    for r in range(rounds):
        order = settings[r % len(settings):] + settings[:r % len(settings)]
        for name, *_ in order:
            c, journal = conns[name]
            for k in range(per_round):
                t = time.perf_counter()
                c.execute("BEGIN IMMEDIATE")
                c.execute("INSERT INTO items VALUES (?, ?, ?) ON CONFLICT (id) DO UPDATE SET "
                          "record = excluded.record, strokes = excluded.strokes",
                          (k % 30, "y" * 560, os.urandom(2000)))
                c.execute("COMMIT")
                commits[name].append((time.perf_counter() - t) * 1000)
                time.sleep(0.05)
            if journal == "WAL":
                t = time.perf_counter()
                c.execute("PRAGMA wal_checkpoint(TRUNCATE)")
                checkpoints[name].append((time.perf_counter() - t) * 1000)
finally:
    if hog is not None:
        hog.terminate()
        hog.wait()
    for c, _ in conns.values():
        c.close()

def summary(times):
    s = sorted(times)
    q = lambda p: s[min(len(s) - 1, int(len(s) * p))]
    return {"n": len(s), "median": statistics.median(s), "p90": q(0.9), "p99": q(0.99), "max": s[-1],
            "over8": sum(t > 8.3 for t in s), "over33": sum(t > 33.3 for t in s), "over100": sum(t > 100 for t in s)}

result = {"condition": condition,
          "commits": {n: summary(t) for n, t in commits.items()},
          "checkpoints": {n: summary(t) for n, t in checkpoints.items()}}
out = os.path.join(base, f"journal-{condition}.json")
json.dump(result, open(out, "w"), indent=1)
print(f"== {condition}: commits (ms), n={rounds * per_round} each")
print(f"{'setting':24s} {'median':>8s} {'p90':>8s} {'p99':>8s} {'max':>8s} {'>8.3':>6s} {'>33':>5s} {'>100':>5s}")
for n, s in result["commits"].items():
    print(f"{n:24s} {s['median']:8.2f} {s['p90']:8.2f} {s['p99']:8.2f} {s['max']:8.1f} {s['over8']:6d} {s['over33']:5d} {s['over100']:5d}")
print(f"-- checkpoints after each round of {per_round} commits (ms)")
for n, s in result["checkpoints"].items():
    print(f"{n:24s} {s['median']:8.2f} {s['p90']:8.2f} {'':8s} {s['max']:8.1f}")
