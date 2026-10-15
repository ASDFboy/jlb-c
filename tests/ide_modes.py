#!/usr/bin/env python3
"""The editor modes: --check, --ide and --repl. These match the Rust interpreter's
tests (jlb-rust/tests/ide_modes.rs), so both interpreters answer editors the same way.

    python3 tests/ide_modes.py ./jlb-c
"""

import os
import subprocess
import sys
import tempfile

EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./jlb-c")
M = "\x1e"
failures = []


def jlb(args, stdin=""):
    r = subprocess.run([EXE] + args, input=stdin.encode(), capture_output=True, timeout=30)
    norm = lambda b: b.decode("utf-8", "replace").replace("\r\n", "\n")
    return norm(r.stdout), norm(r.stderr), r.returncode


def check(name, cond, detail=""):
    if not cond:
        failures.append(f"{name}: {detail}")


def check_mode():
    out, _, _ = jlb(["--check", "-"], "PRINT(1)\n")
    check("check ok", out == '{"ok":true}\n', out)
    out, _, code = jlb(["--check", "-"], "x = 5\ny = x != 3\n")
    e = '{"message":"!= isn\'t an operator in .jlb; use !== (or !=== for the strict version)","line":2,"col":7}'
    want = '{"ok":false,"message":"!= isn\'t an operator in .jlb; use !== (or !=== for the strict version)","line":2,"col":7,"errors":[' + e + "]}\n"
    check("check error", code == 1 and out == want, out)
    # Every problem, not just the first.
    out, _, _ = jlb(["--check", "-"], "x = 5\nIF x != 3 {\n    Print(x)\n}\nmyScore = 1\n")
    check("check several", '"line":2,"col":6}' in out and "did you mean PRINT?" in out and "my_score" in out and out.count('"message"') == 4, out)
    out, _, _ = jlb(["--check", "-"], "x = 1\nIF x > 0 {\n  PRINT(x)\n")
    check("check unclosed", '"line":2,"col":10' in out, out)
    out, _, _ = jlb(["--check", "-"], "POINT a\nPOINT a\n")
    check("check no column", '"line":2,"col":null' in out, out)


def ide_mode_crash_report():
    d = tempfile.mkdtemp(prefix="jlb-c-ide-test-")
    f = os.path.join(d, "crash.jlb")
    with open(f, "w") as fh:
        fh.write('FUNC f(n) {\n  LOCAL k = n * 2\n  RETURN k / 0\n}\nname = "Alex"\nf(3)\n')
    out, err, code = jlb(["--ide", f])
    check("ide exit code", code == 1, code)
    check("ide no output", out == "", out)
    lines = [l for l in err.split("\n") if l.startswith(M)]
    p = f.replace("\\", "\\\\")
    want = (
        M + '{"message":"can\'t divide by zero","file":"crash.jlb","path":"' + p + '","line":3,"traced":true,"frames":[{"name":"f","line":3,"path":"' + p
        + '","vars":[{"name":"n","value":"3","type":"NUMBER"},{"name":"k","value":"6","type":"NUMBER"}]}],"outside":[{"name":"name","value":"\\"Alex\\"","type":"TEXT"}]}'
    )
    check("ide json", lines and lines[0] == want, lines)
    check("ide report", err.startswith("ERROR in crash.jlb, line 3: can't divide by zero"), err)


def repl_mode():
    stdin = f'x = 5\n{M}\nx * 2\n{M}\n"25" * 2\n{M}\nPRINT(x)\n{M}\nx / 0\n{M}\n{M}RESET\nx\n{M}\n'
    out, _, code = jlb(["--repl"], stdin)
    lines = out.split("\n")
    check("repl exit", code == 0, code)
    check("repl 0", lines[0] == M + '{"ok":true}', lines[0])
    check("repl 1", lines[1] == M + '{"ok":true,"value":"10","type":"NUMBER"}', lines[1])
    check("repl 2", lines[2] == M + '{"ok":true,"value":"\\"2525\\"","type":"TEXT"}', lines[2])
    check("repl 3", lines[3] == "5", lines[3])
    check("repl 4", lines[4] == M + '{"ok":true}', lines[4])
    check("repl 5", "can't divide by zero" in lines[5], lines[5])
    check("repl 6", lines[6] == M + '{"ok":true}', lines[6])
    check("repl 7", "x isn't defined" in lines[7], lines[7])


def repl_input_asks_the_editor():
    out, _, _ = jlb(["--repl"], f'n = INPUT("Name? ")\n{M}\nAlex\nn * -1\n{M}\n')
    lines = out.split("\n")
    check("input 0", lines[0] == M + '{"input":"Name? "}', lines[0])
    check("input 1", lines[1] == M + '{"ok":true}', lines[1])
    check("input 2", lines[2] == M + '{"ok":true,"value":"\\"xelA\\"","type":"TEXT"}', lines[2])


for t in (check_mode, ide_mode_crash_report, repl_mode, repl_input_asks_the_editor):
    t()
if failures:
    print("\n".join(failures))
    print(f"{len(failures)} checks failed")
    sys.exit(1)
print("all editor-mode checks passed")
