# `scripts/` — build & harness helpers (owned by Kiln · Platform)

## `new-session.ps1` — a fresh model session, chosen model, chosen directory

The WAVE-5 §0 cost rule ("frontier model, fresh context — never escalate a
long-lived session") as a button instead of a workaround (item 124).

**The measured numbers it is built on** (WAVE-5 §0, operator probes):

| Probe | Frontier (claude-opus-5-5) | Default (opencode-go/gpt-6-luna) |
| ----- | -------------------------- | -------------------------------- |
| one-word reply | **$3.28** | **$0.095** (34x cheaper) |
| same work, 15-min window | — | $0.75 vs kimi-k3 $4.76 (6.4x) |

Why: every step of a long-lived session re-reads ~700k tokens of cached
context. The context, not the output, is what you pay for — so pay frontier
prices only while the context is small, on a session that starts fresh.

### Usage

```powershell
# 1. see the model ids (provider/model):
.\scripts\new-session.ps1 -ListModels

# 2. fresh interactive session, model fixed AT LAUNCH, in this repo (default dir):
.\scripts\new-session.ps1 -Model opencode-go/gpt-6-luna

# 3. fresh session in a DIFFERENT directory:
.\scripts\new-session.ps1 -Model opencode-go/gpt-6-luna -Directory C:\src\other-project

# 4. one-shot headless run (for scripts/CI) — still a fresh session:
.\scripts\new-session.ps1 -Model opencode-go/gpt-6-luna -Run "Reply with OK only."

# 5. full-screen TUI in the directory (see limitation below):
.\scripts\new-session.ps1 -Model opencode-go/gpt-6-luna -FullTui

# 6. see the exact command without launching anything (receipts, CI):
.\scripts\new-session.ps1 -Model opencode-go/gpt-6-luna -PrintOnly
```

### Fresh by construction

The script **never** passes `--continue`; a session starts new unless you
deliberately hand it a `-SessionId` to create. Resume flags are deliberately
absent from the surface — resuming the old context is the thing this button
exists to avoid.

### Known limitation (measured, not assumed)

Interactive default is **`opencode mini`**, because in OpenCode v2.0.28 the
root/TUI command has **no `--model` flag** (checked: root flags are
standalone/server/auto/continue/session/prompt). `mini` and `run` both take
`-m provider/model`, so the model is fixed *at launch* there. `-FullTui` opens
the full TUI in the directory and you pick the model in its model picker — the
script prints the id to choose. If a future OpenCode adds `--model` to the
root command, switch the default branch and delete this paragraph.

### Where the CLI comes from

Looks for `opencode` / `opencode-cli` on PATH first, then the desktop app's
bundled binary
(`%LOCALAPPDATA%\Programs\@opencodedesktop\resources\opencode-cli.exe`,
measured present, v2.0.28).

## `smoke_gate.ps1`

CTest runner for `app.smoke_size_agree` (item 102): runs the bounded smoke,
reports exit 77 (CTest SKIP) when there is no GL context, passes real failures
through unchanged — the mechanism that lets the smoke run in every default
`ctest` invocation without turning context-free machines red.
