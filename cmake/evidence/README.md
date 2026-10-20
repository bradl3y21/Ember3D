# Evidence declarations — authoring rules

One file per item, `cmake/evidence/<name>.evidence`, verified mechanically by
`cmake/VerifyEvidence.cmake` (D33, ARCHITECTURE §7). Drop a file here, get an
`evidence.<name>` ctest case, wired through the build fixture.

```
item:  <id or slug>                     required
class: BUILD|SCENE|RENDER|EDITOR|DECISION|DOC   required
claim: <one line — what this evidence asserts>  required
residue: <what it does NOT prove>                required (check 6 refuses empty)
```

## Keys by check

| Check | Keys |
| ----- | ---- |
| 1 freshness | none — runs automatically, gates everything else |
| 2/3 artifact | `artifact:` **requires** `depends:` (D38). `depends:` entries are FILES or DIRECTORIES; a directory means everything under it, recursively. Literal globs (`src/**`) are rejected — name the directory. |
| 4 test | `test:` — the ctest case name; it must appear in `ctest -N` AND exit 0 |
| 5 symbol | `symbol:` + `symbol_file:`, optionally `test_file:`, and for EDITOR class a non-test `call_site_file:` is REQUIRED |
| 6 residue | `residue:` — non-empty, reprinted on success |
| RENDER | `run_log:`, `shader:`, `measured:` (needs ≥2 numbers) |
| DOC | `doc_must_contain:` (repeatable — ALL must be found), `doc_must_not_contain:` (repeatable phrase set), `doc_must_not_contain_regex:` (repeatable) |
| DECISION | `decision_id:` — names the id; the FREEZE is not checked (no authenticated HTTP) — stated as residue in-check |
| D36 port | `port_file:` + `origin:` (repeatable) — verbatim SPDX pair + `Origin: blender/blender …` + `Modifications:` in the first 40 lines |

## The two sharp edges that bite (measured, not theorised)

1. **`symbol` must appear literally, in CODE.** Check 5 strips `//` and `/* */`
   before matching (a comment is not a definition), and then does a literal
   substring search. So:
   - Point `symbol_file` at the **`.cpp` with the out-of-line definition** —
     `EditableMesh.h` does NOT contain the substring `EditableMesh::validate`
     while its `.cpp` does.
   - A free function inside `namespace demo` never emits `demo::spin` as a
     substring — qualify exactly as the source spells it, or use the
     definition file's own spelling.
   - A test referencing `w.spin(3)` does NOT contain `Widget::spin`. The form
     that does: bind a member pointer, `&Widget::spin` — the only spelling
     that emits the literal.
   - String literals are NOT stripped: a symbol in a quoted string still
     matches (false-pass direction, known residue).

2. **DOC stale claims: enumerate, and mind the regex dialect.** One verbatim
   string is a loop (the same claim reworded walks through it — ROADMAP.md
   had it twice in two phrasings). List every known phrasing as a repeatable
   `doc_must_not_contain:` and add a `doc_must_not_contain_regex:` for the
   inflections. CMake uses Spencer regexes: **`{n,m}` intervals are not
   supported and silently never match** — use `[^.]*` or `.*`. Keep regexes
   scoped: one that also matches the CORRECTED text fails forever.

## Non-goals (named so nobody mistakes a pass)

- A complete declaration can still be **false** — the verifier checks
  declarations against the tree, not the world. Undeclared claims are
  invisible (same shape as undeclared ports).
- The freshness check cannot see a **mixed** tree (one relinked binary
  newer than the newest source passes) — the per-target variant is the
  follow-up.
- DECISION frozen-ness is not verified (CMake has no authenticated HTTP).
