#!/usr/bin/env python3
"""Exercise the real terminal, SQLite, text snapshots and crash recovery.

Every scenario owns an isolated temporary directory. No test uses live data/.
Only Python's standard library is required.
"""
import concurrent.futures
import contextlib
import fcntl
import hashlib
import hmac
import json
import os
from pathlib import Path
import pty
import random
import select
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "teller-system"
CRASH_BINARY = ROOT / "build/teller-test"
PASSWORD = "q1w2e3r4t5y6"  # Public, fictional starter credentials.


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def seed(parent, name):
    directory = parent / name
    directory.mkdir()
    for file in ("users.txt", "records.txt", "transactions.txt"):
        shutil.copyfile(ROOT / "data" / file, directory / file)
    return directory


def run(directory, lines=None, args=(), binary=BINARY, env=None, timeout=20):
    text = "\n".join(map(str, lines)) + "\n" if lines is not None else ""
    return subprocess.run([str(binary), "--data-dir", str(directory), *args],
                          input=text, text=True, capture_output=True,
                          timeout=timeout, env=env, check=False)


def successful(result):
    require(result.returncode == 0, f"exit {result.returncode}: {result.stderr}\n{result.stdout[-2000:]}")
    require(not result.stderr, result.stderr)
    return result.stdout


def logged(*actions, user="Alice", password=PASSWORD):
    return ["1", user, password, *actions, "0"]


def creation(number, kind="savings", amount="1001.20"):
    return ["1", "10/10/2012", str(number), "UK", "291231392", amount, kind]


def transaction(number, kind, amount):
    return ["5", str(number), "1" if kind == "deposit" else "2", amount]


def query(directory, sql, parameters=()):
    with contextlib.closing(sqlite3.connect(directory / "teller.db")) as db:
        return db.execute(sql, parameters).fetchall()


def money(cents):
    return f"{cents // 100}.{cents % 100:02d}"


def snapshots_match(directory):
    users = query(directory, "SELECT id,name,password FROM users ORDER BY id")
    expected = "".join(f"{i} {name} {password}\n" for i, name, password in users)
    require((directory / "users.txt").read_text() == expected, "users projection differs")
    records = query(directory, "SELECT a.record_id,a.owner,u.name,a.number,a.created,a.country,a.phone,a.balance,a.kind FROM accounts a JOIN users u ON u.id=a.owner WHERE active=1 ORDER BY record_id")
    expected = "".join(" ".join(map(str, (*row[:7], money(row[7]), row[8]))) + "\n" for row in records)
    require((directory / "records.txt").read_text() == expected, "records projection differs")
    transactions = query(directory, "SELECT t.account_id,t.kind,t.amount,t.day FROM transactions t JOIN accounts a ON a.number=t.account_id WHERE active=1 ORDER BY t.id")
    expected = "".join(f"{a} {kind} {money(amount)} {day}\n" for a, kind, amount, day in transactions)
    require((directory / "transactions.txt").read_text() == expected, "transactions projection differs")
    require(query(directory, "PRAGMA integrity_check") == [("ok",)], "SQLite integrity")
    require(not query(directory, "PRAGMA foreign_key_check"), "foreign keys")
    for _, name, encoded in users:
        require(encoded.startswith("pbkdf2-sha256$600000$"), f"unhashed password for {name}")
        require(PASSWORD not in encoded, "plaintext password persisted")
    return len(transactions)


class Session:
    def __init__(self, directory):
        self.process = subprocess.Popen([str(BINARY), "--data-dir", str(directory)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0)
        self.output = b""
        self.cursor = 0

    def send(self, *lines):
        self.process.stdin.write(("\n".join(map(str, lines)) + "\n").encode())
        self.process.stdin.flush()

    def expect(self, text, timeout=10):
        target = text.encode()
        deadline = time.monotonic() + timeout
        while True:
            index = self.output.find(target, self.cursor)
            if index >= 0:
                self.cursor = index + len(target)
                return
            remaining = deadline - time.monotonic()
            require(remaining > 0, f"waiting for {text!r}: {self.output[-1500:]!r}")
            ready, _, _ = select.select([self.process.stdout], [], [], min(remaining, .2))
            if ready:
                chunk = os.read(self.process.stdout.fileno(), 65536)
                require(chunk, f"process ended before {text!r}: {self.output[-1500:]!r}")
                self.output += chunk

    def login(self, user="Alice", password=PASSWORD):
        self.expect("> ")
        self.send("1", user, password)
        self.expect(f"Welcome, {user}.")
        self.expect("> ")

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.stdin = None
            self.process.communicate(timeout=10)
        else:
            self.process.communicate(timeout=10)
        require(self.process.returncode == 0, f"session exit {self.process.returncode}")


def functional(parent):
    directory = seed(parent, "functional")
    text = successful(run(directory, ["2", "Marcus", PASSWORD, "2", "Alice", PASSWORD,
                                      "2", "Laura", PASSWORD, "3"]))
    require("User already exists" in text, "duplicate Alice accepted")
    require(query(directory, "SELECT COUNT(*) FROM users WHERE name='Marcus'") == [(1,)], "Marcus missing")
    for _, encoded in query(directory, "SELECT name,password FROM users"):
        _, rounds, salt, digest = encoded.split("$")
        actual = hashlib.pbkdf2_hmac("sha256", PASSWORD.encode(), bytes.fromhex(salt), int(rounds)).hex()
        require(hmac.compare_digest(actual, digest), "password hash cannot verify independently")
    accounts = [(834213, "savings"), (320421, "fixed01"), (3214, "fixed02"), (3212, "fixed03"), (9999, "current")]
    commands = []
    for number, kind in accounts:
        commands += creation(number, kind) + ["3", str(number)]
    text = successful(run(directory, logged(*commands)))
    for expected in ["$5.84 as interest on day 10 of every month", "$40.05 as interest on 10/10/2013",
                     "$100.12 as interest on 10/10/2014", "$240.29 as interest on 10/10/2015",
                     "You will not get interests because the account is of type current",
                     "No transaction history available."]:
        require(expected in text, f"missing {expected}")
    text = successful(run(directory, logged("2", "444444", "2", "834213", "1", "+77001234567",
                                              "2", "834213", "2", "Kazakhstan", "3", "834213")))
    require("Account does not exist" in text and "phone number or [2] country" in text, "update selection")
    require("Phone: +77001234567" in text and "Country: Kazakhstan" in text, "update not shown")
    text = successful(run(directory, logged("5", "320421", "5", "3214", "5", "3212")))
    require(text.count("Cannot withdraw or deposit for fixed accounts.") == 3, "fixed transactions allowed")
    before = (directory / "transactions.txt").read_bytes()
    commands = transaction(834213, "deposit", "500.00") + transaction(834213, "deposit", "200.00")
    commands += transaction(834213, "withdrawal", "100.00") + ["3", "834213"]
    text = successful(run(directory, logged(*commands)))
    require("Spending pattern: saver" in text and "2 deposits ($700.00) | 1 withdrawals ($100.00) | ratio: 88%" in text, "analyzer bonus")
    require("Balance: $1601.20" in text, "incorrect balance")
    require((directory / "transactions.txt").read_bytes().startswith(before), "new transactions did not append logically")
    text = successful(run(directory, logged(*transaction(834213, "withdrawal", "1601.21"))))
    require("Insufficient funds" in text and snapshots_match(directory) == 3, "overdraft changed history")

    recipient = Session(directory)
    try:
        recipient.login("Laura")
        started = time.monotonic()
        successful(run(directory, logged("7", "3212", "Laura", "YES")))
        recipient.expect("[Notification] Account 3212 transferred to you by Alice.", timeout=2)
        notification_ms = round((time.monotonic() - started) * 1000, 3)
        require(notification_ms < 2000, "notification was not live")
        recipient.send("3", "3212")
        recipient.expect("Owner: Laura")
        recipient.expect("$240.29 as interest on 10/10/2015")
    finally:
        recipient.close()
    text = successful(run(directory, logged("3", "3212", "5", "2")))
    require(text.count("Account does not exist for this user.") == 2, "ownership isolation")
    commands = []
    for number in (834213, 320421, 3214):
        commands += ["6", str(number), "YES"]
    commands += ["6", "444444"]
    text = successful(run(directory, logged(*commands)))
    require("Account does not exist" in text, "missing account removal")
    require(query(directory, "SELECT COUNT(*) FROM accounts WHERE number IN(834213,320421,3214) AND active=1") == [(0,)], "deleted accounts remain active")
    require(query(directory, "SELECT COUNT(*) FROM transactions WHERE account_id=834213") == [(3,)], "archived history lost")
    require(snapshots_match(directory) == 0, "archived operations leaked into active text view")
    successful(run(directory, args=("--check",)))
    return {"audit_accounts": 5, "notification_ms_including_sender_login": notification_ms}


def labels(parent):
    vectors = [(596, 404, 60, "saver"), (594, 406, 59, "moderate"),
               (595, 405, 60, "saver"), (395, 605, 40, "moderate"),
               (394, 606, 39, "spender"), (800, 200, 80, "saver"),
               (1, 0, 100, "saver"), (0, 1, 0, "spender")]
    for index, (deposits, withdrawals, ratio, label) in enumerate(vectors):
        directory = seed(parent, f"ratio-{index}")
        rows = []
        if deposits:
            rows.append(f"0 deposit {money(deposits)} 01/10/2026\n")
        if withdrawals:
            rows.append(f"0 withdrawal {money(withdrawals)} 01/10/2026\n")
        (directory / "transactions.txt").write_text("".join(rows))
        text = successful(run(directory, logged("3", "0")))
        require(f"Spending pattern: {label}" in text and f"ratio: {ratio}%" in text, "rounding/classification mismatch")
        snapshots_match(directory)
    return len(vectors)


def crash_recovery(parent):
    points = {"after-balance": False, "before-commit": False, "after-commit": True, "after-publish": True}
    for point, committed in points.items():
        directory = seed(parent, "crash-" + point)
        successful(run(directory, args=("--check",)))
        before = query(directory, "SELECT balance FROM accounts WHERE number=0")[0][0]
        env = os.environ.copy()
        env["TELLER_FAILPOINT"] = point
        result = run(directory, logged(*transaction(0, "deposit", "12.34")), binary=CRASH_BINARY, env=env)
        require(result.returncode == 86, f"failpoint {point} not reached: {result.stderr}")
        successful(run(directory, args=("--check",)))
        expected = before + (1234 if committed else 0)
        require(query(directory, "SELECT balance FROM accounts WHERE number=0") == [(expected,)], f"wrong recovery at {point}")
        require(snapshots_match(directory) == int(committed), f"orphan/missing history at {point}")
        require(len(list((directory / ".snapshots").iterdir())) == 1, "orphan snapshots not collected")
    return len(points)


def malformed_imports(parent):
    users = (ROOT / "data/users.txt").read_bytes()
    cases = [
        ("users.txt", users + users.splitlines(keepends=True)[0]),
        ("users.txt", b"2147483648 Bad password\n"),
        ("users.txt", b"0 Alice pass\x00hidden\n"),
        ("records.txt", b"0 9 Alice 0 10/10/2012 UK 12345 1.00 savings\n"),
        ("records.txt", b"0 0 Alice 0 31/02/2012 UK 12345 1.00 savings\n"),
        ("records.txt", b"0 0 Alice 0 10/10/2012 UK 12345 -1.00 savings\n"),
        ("transactions.txt", b"999 deposit 1.00 01/10/2026\n"),
        ("transactions.txt", b"0 deposit NaN 01/10/2026\n"),
        ("transactions.txt", b"0 withdrawal 0.00 01/10/2026\n"),
        ("transactions.txt", b"2 deposit 1.00 01/10/2026\n"),
        ("transactions.txt", b"0 deposit 999999999.00 01/10/2026\n"),
        ("records.txt", b"x" * 600 + b"\n"),
    ]
    for index, (file, data) in enumerate(cases):
        directory = seed(parent, f"malformed-{index}")
        (directory / file).write_bytes(data)
        originals = {name: (directory / name).read_bytes() for name in ("users.txt", "records.txt", "transactions.txt")}
        result = run(directory, args=("--check",))
        require(result.returncode == 1 and "Storage error:" in result.stderr, f"malformed import accepted: {index}")
        for name, original in originals.items():
            require((directory / name).read_bytes() == original, f"failed import modified {name}")
    legacy = seed(parent, "legacy")
    (legacy / "users.txt").write_text(f"0 Alice {PASSWORD}\n1 Michel {PASSWORD}\n")
    records = (legacy / "records.txt").read_text().replace("savings", "saving").replace("10023.23", "10023.230000")
    (legacy / "records.txt").write_text("\n" + records + "\n")
    text = successful(run(legacy, logged("4")))
    require("Welcome, Alice" in text, "legacy login migration")
    snapshots_match(legacy)
    return len(cases)


def stress(parent):
    directory = seed(parent, "stress")
    successful(run(directory, args=("--check",)))
    balance = query(directory, "SELECT balance FROM accounts WHERE number=0")[0][0]
    randomizer = random.Random(2008)
    commands, count = [], 0
    for index in range(120):
        amount = randomizer.randint(1, 50000)
        kind = "deposit" if index % 3 else "withdrawal"
        if index % 17 == 0:
            amount, kind = balance + 1, "withdrawal"
        commands += transaction(0, kind, money(amount))
        if kind == "deposit":
            balance += amount
            count += 1
        elif amount <= balance:
            balance -= amount
            count += 1
    successful(run(directory, logged(*commands), timeout=60))
    require(query(directory, "SELECT balance FROM accounts WHERE number=0") == [(balance,)], "reference ledger disagrees")
    require(snapshots_match(directory) == count, "invalid operations entered history")

    # Sixteen competing withdrawals from $100: exactly ten can succeed.
    successful(run(directory, logged(*creation(900, "current", "100.00"))))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        outputs = list(pool.map(lambda _: successful(run(directory, logged(*transaction(900, "withdrawal", "10.00")))), range(16)))
    require(sum("Insufficient funds" in text for text in outputs) == 6, "concurrent overdraft detection")
    require(query(directory, "SELECT balance FROM accounts WHERE number=900") == [(0,)], "concurrent lost update")
    require(query(directory, "SELECT COUNT(*) FROM transactions WHERE account_id=900") == [(10,)], "concurrent history not atomic")
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        outputs = list(pool.map(lambda _: successful(run(directory, ["2", "Unique", PASSWORD, "3"])), range(6)))
    require(query(directory, "SELECT COUNT(*) FROM users WHERE name='Unique'") == [(1,)], "duplicate concurrent user")
    require(sum("User already exists" in text for text in outputs) == 5, "concurrent registration feedback")
    snapshots_match(directory)
    return {"model_operations": 120, "accepted_model_operations": count, "competing_withdrawals": 16, "simultaneous_workers": 8, "concurrent_registrations": 6}


def failures(parent):
    directory = seed(parent, "failures")
    successful(run(directory, args=("--check",)))
    # A damaged projection is reconstructed from SQLite without losing money.
    (directory / "records.txt").write_text("damaged\n")
    successful(run(directory, args=("--check",)))
    snapshots_match(directory)
    session = Session(directory)
    try:
        session.login()
        (directory / ".snapshots").rename(directory / ".snapshots-hold")
        (directory / ".snapshots").write_text("blocked")
        session.send(*transaction(0, "deposit", "1.00"))
        session.expect("Snapshot path must be a real directory.")
        require(query(directory, "SELECT COUNT(*) FROM transactions") == [(0,)], "I/O failure changed money")
        (directory / ".snapshots").unlink()
        (directory / ".snapshots-hold").rename(directory / ".snapshots")
    finally:
        session.close()
    successful(run(directory, args=("--check",)))
    with (directory / ".lock").open("rb") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        started = time.monotonic()
        result = run(directory, args=("--check",), timeout=8)
        require(result.returncode == 1 and "busy in another session" in result.stderr, "lock timeout")
        require(time.monotonic() - started < 7, "lock timeout too slow")
    fifo_dir = seed(parent, "fifo")
    (fifo_dir / "transactions.txt").unlink()
    os.mkfifo(fifo_dir / "transactions.txt")
    result = run(fifo_dir, args=("--check",), timeout=3)
    require(result.returncode == 1, "FIFO input blocked/accepted")
    corrupt = seed(parent, "corrupt-db")
    (corrupt / "teller.db").write_bytes(b"not a database")
    require(run(corrupt, args=("--check",)).returncode == 1, "corrupt database accepted")
    with sqlite3.connect(directory / "teller.db") as db:
        db.execute("UPDATE accounts SET balance=balance+1 WHERE number=0")
    result = run(directory, args=("--check",))
    require(result.returncode == 1 and "inconsistent" in result.stderr, "ledger corruption undetected")
    return 6


def terminal_safety(parent):
    directory = seed(parent, "terminal")
    successful(run(directory, args=("--check",)))
    text = successful(run(directory, ["x" * 10000, "3"]))
    require("Input is too long" in text, "long input not handled")
    for lines in [[], ["1"], ["1", "Alice"], logged("1", "10/10/2012")[:-1]]:
        require(run(directory, lines).returncode == 0, "EOF did not exit cleanly")
    master, slave = pty.openpty()
    process = subprocess.Popen([str(BINARY), "--data-dir", str(directory)], stdin=slave,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    try:
        os.write(master, b"1\nAlice\n")
        output = b""
        deadline = time.monotonic() + 10
        while b"Password: " not in output:
            require(time.monotonic() < deadline, "password prompt timed out")
            ready, _, _ = select.select([process.stdout], [], [], .2)
            if ready:
                chunk = os.read(process.stdout.fileno(), 65536)
                require(chunk, "terminal ended early")
                output += chunk
        require(not termios.tcgetattr(slave)[3] & termios.ECHO, "password echo remains enabled")
        process.send_signal(signal.SIGTERM)
        process.communicate(timeout=5)
        require(process.returncode == 143, "signal exit status")
        require(termios.tcgetattr(slave)[3] & termios.ECHO, "terminal echo not restored")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)
    return {"overlong_input_bytes": 10000, "eof_cases": 4, "password_echo_and_sigterm": "PASS"}


def main():
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="teller-audit-") as temporary:
        parent = Path(temporary)
        result = {"functional": functional(parent), "ratio_cases": labels(parent),
                  "crash_boundaries": crash_recovery(parent), "malformed_imports": malformed_imports(parent),
                  "stress": stress(parent), "storage_failure_cases": failures(parent),
                  "terminal": terminal_safety(parent)}
    result.update(status="PASS", elapsed_seconds=round(time.monotonic() - started, 3))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
