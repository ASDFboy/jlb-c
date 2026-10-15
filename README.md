<img src="https://raw.githubusercontent.com/ASDFboy/jlb-rust/main/docs/images/jlb-logo.svg" width="112" alt="The JLB logo">

# jlb-c

The interpreter for **.jlb**, a small programming language, written in C.

**Authors:** [ASDFboy](https://github.com/ASDFboy) and Claude

It's the second interpreter for .jlb, next to [jlb-rust](https://github.com/ASDFboy/jlb-rust). The two follow the same rules, give the same error messages and print the same output, and both must pass the same test programs in [`tests/cases`](tests/cases). The language itself is described in [jlb-rust's guide](https://github.com/ASDFboy/jlb-rust/tree/main/docs).

```text
>> hello.jlb
name = INPUT("What's your name? ")
PRINT("Hello, " + name + "!")
```

```text
$ jlb-c hello.jlb
What's your name? Sam
Hello, Sam!
```

## Why a second interpreter?

- **It keeps the language honest.** When two interpreters written separately agree on every test program, the rules are written down clearly enough to be built twice, and a difference between them points straight at a bug in one of them.
- **It's small and quick.** One small program with no dependencies (about 200 KB on Linux, 530 KB on Windows), built from plain C11. A million-step loop takes about 0.12 seconds.
- **It's easy to build anywhere** there's a C compiler, including computers Rust doesn't support.

[JLB IDE](https://github.com/ASDFboy/jlb-ide) can run programs with either interpreter (switch with one click in its toolbar) and compare the two.

## Using it

| Command | What it does |
| --- | --- |
| `jlb-c file.jlb` | runs a program |
| `jlb-c --pause file.jlb` | runs it, then waits for Enter (so the window stays open) |
| `jlb-c --check file.jlb` | checks the syntax without running anything (`-` reads the code from input) |
| `jlb-c --ide file.jlb` | runs it for an editor: output is sent right away, and a crash adds a JSON report |
| `jlb-c --repl` | starts a Quick-try session for an editor |
| `jlb-c --version` | shows the version |

The editor modes (`--check`, `--ide` and `--repl`) are described in [jlb-rust's guide](https://github.com/ASDFboy/jlb-rust/blob/main/docs/09-programs-and-files.md#editor-modes). jlb-c offers all of them except `--debug`, so JLB IDE's step-by-step debugger uses the Rust interpreter.

On Windows, a double-clicked program keeps its window open until you press Enter. To make .jlb files run when double-clicked, right-click one, choose **Open with → Choose another app**, browse to `jlb-c.exe` and tick **Always** (or use jlb-rust's [one-click setup](https://github.com/ASDFboy/jlb-rust#windows-quickest), which sets up the Rust interpreter).

## Getting it

**Windows:** download [`windows/jlb-c.exe`](windows/jlb-c.exe) (click it, then **Download raw file**). It's not code-signed (a certificate costs money every year), so SmartScreen may warn the first time: choose **More info → Run anyway**.

**Building it yourself:** you need a C compiler (GCC or Clang, both free) and `make`.

```sh
make            # builds ./jlb-c
make test       # runs the shared test programs
make windows    # builds windows/jlb-c.exe (needs MinGW-w64)
```

Without `make`, this does the same:

```sh
cc -O2 -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -o jlb-c src/*.c -lm -lpthread
```

## Tests

- `tests/run.sh ./jlb-c` runs every program in `tests/cases` and compares its output with the matching `.out` file (and feeds in a `.in` file as typed input, if there is one). The cases are the same ones jlb-rust uses; when one changes there, copy it here.
- `python3 tests/ide_modes.py ./jlb-c` checks the editor modes, matching jlb-rust's `tests/ide_modes.rs`.

## How it's built

| File | What's in it |
| --- | --- |
| `src/lex.c`, `src/parse.c` | turning text into tokens, then into a syntax tree |
| `src/num.c` | exact numbers of any size and fractions (with floating point only where answers can't be exact) |
| `src/value.c` | values, tables (copy-on-write, with `@` sharing), errors and the crash report |
| `src/ops.c` | operators and the truth rules |
| `src/interp.c` | running the tree: variables, loops, functions, `TRY`, `GOTO`, `IMPORT` and `LOAD` |
| `src/builtins.c` | the ALL CAPS commands |
| `src/ide.c` | the editor modes |
| `src/main.c` | the command line |

Memory is managed with reference counting, like the Rust version's `Rc`. Programs run on a thread with a large stack, so deep recursion works the same in both.

## Known differences

- `UPPER` and `LOWER` use the C library's letter tables, plus Unicode's special cases (`ß` → `SS`, a final `Σ` → `ς`). Rare letters can come out differently from the Rust version.
- Error messages from the operating system (like a missing file in `READ`) use the C library's wording, which matches Rust on Linux and macOS but can differ on Windows.
