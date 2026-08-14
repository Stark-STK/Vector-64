# vector64 Engine: Response to Integration Requirements

Reply to `ENGINE_INTEGRATION_REQUIREMENTS.md`, from the engine side.

Branch: `STK/four-player-game-support-112931`. Engine-side design detail lives
in [variants.md](variants.md); this document answers the requirements
point by point and lists what the server team still needs to decide.

**Headline:** 12 of the 12 checklist items are implemented. Three carry
caveats you need to read (sections 3.2, 4.3, 7.3), and one requirement was
implemented *differently* from how the doc describes it, because the doc's
description would have produced wrong bughouse games (section 3.2).

---

## 0. What to build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Three binaries matter to the server:

| Binary | Tier | Notes |
|---|---|---|
| `ChessEngine-validator` | Validator | No search, no net, 1 MB table, one thread. `go`/`bench` refused. |
| `ChessEngine-variants` | Search | Full search. `--validator` starts it in validator mode instead. |
| `ChessEngine` / `ChessEngine-nnue` | Standard chess only | Tournament binaries. No variant support by design; do not use them for crazyhouse or bughouse. |

The variant code is compile-time gated, so the tournament binaries contain
none of it. That is deliberate and load-bearing: it is why variant work cannot
regress standard strength.

---

## 1. Two capability tiers

**Agreed and implemented as specified.**

`ChessEngine-validator` is the lean tier. It starts with a 1 MB transposition
table and one thread, loads no net, and **refuses `go` and `bench` outright**:

```
> go depth 4
info string refused: this is a validator-only worker
```

That refusal is the enforcement of your "a validator request must not trigger a
search". It is not advisory; a misrouted search request fails loudly instead of
consuming the worker.

Startup allocates the attack tables and a 1 MB TT and nothing else. There is no
net to load, so validator startup is not gated on disk or memory for weights.

---

## 2. Validator API

**Implemented.** All replies are a single line of compact JSON. Requests are
line-oriented text on stdin.

The position is established with the existing `position` command (standard UCI
grammar, which the drop variants extend with `[...]` reserves and `P@e4` drop
notation), then queried. Two lines per request, no game identity held by the
worker.

### 2.1 Validate and apply

```
> setoption name UCI_Variant value crazyhouse
> position startpos moves e2e4 d7d5 e4d5
> apply d8d5
{"ok":true,"legal":true,"fen":"rnb1kbnr/ppp1pppp/8/3q4/8/8/PPPP1PPP/RNBQKBNR[Pp] w KQkq - 0 3","sideToMove":"w","inCheck":false,"terminal":"none","capturedDropType":"p"}
```

Fields map onto your section 2.1 one for one:

- `legal` -- always `true` on an `ok` reply; an illegal move is an error reply.
- `fen` -- the resulting position, reserves included. Commit exactly this.
- `sideToMove` -- `"w"` or `"b"`, after the move.
- `inCheck` -- whether the new side to move is in check.
- `terminal` -- one of `none`, `checkmate`, `stalemate`, `draw_fifty_move`,
  `draw_threefold`, `draw_insufficient_material`.
- `capturedDropType` -- `"p"`,`"n"`,`"b"`,`"r"`,`"q"`, or `null` when the move
  captured nothing. **Already promotion-reverted**: a captured promoted queen
  reports `"p"`. Verified by test.

### 2.2 Generate legal moves

```
> legalmoves
{"ok":true,"count":28,"moves":["e2e4","e7e8q","e7e8r","P@e4", ...]}
```

Exact UCI strings, one per distinct move. Promotions are enumerated separately
(`e7e8q`, `e7e8r`, `e7e8b`, `e7e8n`), and drops are included in drop variants,
so this maps directly onto your one-capability-per-UCI contract.

### 2.3 Terminal / status query

```
> status
{"ok":true,"sideToMove":"b","inCheck":false,"terminal":"none","legalCount":28,"repetitions":0}
```

`repetitions` is the number of *prior* occurrences of the current position
inside the fifty-move window, so threefold is `repetitions >= 2`. Exposed
separately from `terminal` so you can implement claim-based draws if you ever
want them rather than automatic ones.

### 2.4 Can-side-mate

```
> canmate w
{"ok":true,"side":"w","canMate":true}
```

Semantics: **"could mate be delivered by some legal continuation"** (the FIDE
6.9 sense used for flag adjudication), not "can force mate". So K+N+N reports
`true` -- a helpmate exists even though it cannot be forced. K, K+N and K+B
report `false`.

One deviation from your doc, in your favour: you marked this standard-chess
only. It is implemented for the drop variants too, where **anything in a
reserve is sufficient on its own** -- a bare king with a queen in hand can
still mate. If you adjudicate bughouse or crazyhouse flags, this call is
correct there as well.

### 2.5 Single-line requests

Every validator command may carry its own position as a suffix, so a request
is self-contained in one line and no worker needs to be paired across calls:

```
> legalmoves fen 7k/8/8/8/8/8/8/K5R1[Q] w - - 0 1
> status startpos moves e2e4 e7e5
> apply e4d5 fen <FEN> moves <uci> ...
> canmate b fen <FEN>
```

The clause is `fen <6 fields> [moves ...]` or `startpos [moves ...]`. No move
or side argument can equal `fen` or `startpos`, so the split is unambiguous.
A single-line request never mutates the worker's resident position; the
two-line form still does, so both styles keep working.

### 2.6 Read the position back

Not in your doc, but nothing in section 2 works without it:

```
> getfen
{"ok":true,"fen":"...","variant":"crazyhouse","drawRules":"standard"}
```

Useful for resync and for asserting that the server and engine agree after a
sequence of applies.

---

## 3. Variant support

### 3.1 Coverage

`standard`, `crazyhouse` and `bughouse`, selected with
`setoption name UCI_Variant value chess|crazyhouse|bughouse`. Legality, move
and drop application, and terminal detection are implemented and tested for
all three.

Correctness evidence: crazyhouse perft matches standard chess exactly through
depth 4 (no reserve can be non-empty before a capture, so it must) and gives
4888832 at depth 5 against chess's 4865609 -- which also matches the published
crazyhouse figure. On top of that, 300 randomised games verify after every ply
that hash, reserves, promoted-piece marks, mailbox and material all match a
from-scratch rebuild, and that unmake restores state exactly.

### 3.2 Reserve ownership: please read this one

Your section 3 says bughouse "must accept an injected hand inventory rather than
deriving it solely from its own captures". We went further, because "solely"
would still have been wrong:

> **In bughouse the engine derives *nothing* from its own captures.**

A capture on this board sends the piece to your **partner's** reserve on the
*other* board. This engine sees one board and cannot know about the other, so
a bughouse capture adds nothing to either reserve here. The engine reports
`capturedDropType` and **the server routes it to the partner board's position**.

```
> setoption name UCI_Variant value bughouse
> position fen rnbqkbnr/pppppppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR[Qp] w KQkq - 0 1
> apply e4d5
{"ok":true,...,"fen":"...RNBQKBNR[Qp] b KQkq - 0 1",...,"capturedDropType":"p"}
```

Note the reserve is still `[Qp]` after a pawn capture. Compare crazyhouse,
where the same capture appends to the capturer's own reserve automatically.

This matters because the earlier behaviour -- engine keeps its own captures --
would have silently corrupted every reserve you inject. It was a real bug on
this branch and is now fixed and covered by a test that asserts a bughouse
capture never changes a reserve on its own board.

Consequence for the server: **bughouse hand state is entirely yours.** The
engine never adds to a bughouse reserve; it only spends from one when it plays
a drop. The reserve reaches the engine solely through the `[...]` field of the
FEN you supply, which is why there is no separate hand-injection call -- the
position already carries it.

### 3.3 Bughouse draw rules: a decision you need to make

Real bughouse has neither a repetition nor a fifty-move draw, because the
partner board keeps the game state moving. Your section 3 asks for full standard draw
rules per board. Both are supported; the policy is selectable independently of
the variant:

```
setoption name DrawRules value variant     # default: each variant's own rules
setoption name DrawRules value standard    # force threefold + fifty-move on
```

**To get the behaviour your doc describes, the server must send
`setoption name DrawRules value standard` for bughouse boards.** The default is
`variant`, which for bughouse means `terminal` will never report
`draw_threefold` or `draw_fifty_move`. This is a deliberate default -- an engine
shipped for real bughouse play should not invent draws -- but it is the opposite
of what your adjudicator expects, so it needs to be set explicitly.

Stalemate and insufficient material are reported for bughouse under either
setting.

---

## 4. Statelessness and position representation

### 4.1 Canonical wire format -- proposed, please confirm

```
<board>[<reserve>] <stm> <castling> <ep> <halfmove> <fullmove>
```

- `<board>` is standard FEN board notation, with a trailing `~` on any piece
  that reached its square by promotion: `3Q~3k/8/...`. The mark is what makes
  the promotion revert possible, so it must survive your persistence layer.
- `[<reserve>]` is present in drop variants and absent in standard chess.
  Uppercase letters are White's reserve, lowercase Black's, any order:
  `[QPpp]`. Empty reserve is `[]`.
- The remaining five fields are unchanged from standard FEN.

A standard-variant position **rejects** a FEN carrying a bracket, so a variant
position can never be half-loaded into a standard worker. Tested.

This is the format `setFromFEN`/`toFEN` implement and round-trip; every `fen`
the engine emits is re-readable by the engine. Confirm it and we can call it
pinned.

### 4.2 Threefold via supplied history

Supported through the move-list form your section 4 offers:

```
position fen <start FEN> moves <uci> <uci> ...
```

The engine replays the moves and detects repetition from the resulting history.
`status` then reports `repetitions`.

**Not supported:** passing a list of prior *positions* directly. Your doc
allows either; we implement the move-list form. If your persistence layer
stores positions rather than moves and you would rather not replay, say so and
we will add a position-list form.

### 4.3 Determinism

- **Validator calls are fully deterministic.** Identical
  `(position, moves, variant, draw rules)` always yields an identical result.
  Nothing on that surface searches.
- **Search calls are deterministic only at `Threads 1`.** Lazy SMP is
  non-deterministic by design: helper threads race, so node counts and
  occasionally the chosen move vary run to run. If your replay-based testing
  covers AI moves, pin `setoption name Threads value 1`, and accept the
  strength cost, or record the engine's chosen move rather than expecting to
  reproduce it.

---

## 5. Error taxonomy

**Implemented.** Failures never collapse into one signal:

```
> apply z9z9
{"ok":false,"error":"malformed_request","reason":"unparseable move 'z9z9'"}
> apply e2e4
{"ok":false,"error":"illegal_move","reason":"not legal in this position"}
```

| Code | Meaning | Suggested server action |
|---|---|---|
| `illegal_move` | Well-formed, not legal in this position | Reject and log; keep the worker |
| `malformed_request` | Unparseable move, side, or argument | Terminate the pipeline; keep the worker |
| `internal_error` | Engine fault | Evict and respawn the worker |

**Caveat, stated plainly:** `internal_error` is defined in the protocol but the
engine currently has no path that emits it. An engine fault today surfaces as a
process crash or a hang, not as a JSON reply -- which your Adapter must handle
regardless, since a hard fault cannot be reported by the faulting process. Treat
"no reply within the timeout" and "non-zero exit" as the real internal-error
signals. The code is reserved so that recoverable internal faults can use it
later without a protocol change.

---

## 6. Search API

Implemented over standard UCI on `ChessEngine-variants`:

- **Best move:** `go movetime <ms> | depth <n> | nodes <n>` -> `bestmove <uci>`
  with an optional `ponder <uci>`. Budgets are honoured.
- **Evaluation:** `info ... score cp <n>` or `score mate <n>`, side-relative
  (positive favours the side to move).
- **PV:** `info ... pv <uci> <uci> ...`.
- **MultiPV:** `setoption name MultiPV value <n>`, up to 32.
- **JSON summary:** `setoption name SearchJson value true` adds one structured
  line after `bestmove`, so the Adapter needs no `info`-line scraping:

```
bestmove Q@g7
{"bestMove":"Q@g7","score":{"mate":1},"pv":["Q@g7"],"depth":6}
```

  It is emitted *after* `bestmove`, never instead of it, so plain UCI clients
  are unaffected. `score` carries either `cp` or `mate`, never both.

No book, no networking, no persistence, per your section 6.

**Strength caveat for HvAI.** The engine plays *legal* crazyhouse and bughouse,
not *strong* crazyhouse and bughouse. Drops are currently generated only as
quiet moves, so quiescence never sees a checking drop -- the dominant tactical
motif in drop variants. The engine will hang forced mates. Static exchange
evaluation is also drop-unaware: it does not charge a capture for the material
it donates to the opponent's reserve. Both are tracked as the top items in
[variants.md](variants.md) section 13. If HvAI on drop variants is user-facing
soon, tell us and that work moves ahead of everything else.

NNUE is refused in variant mode (the net has no reserve features and its
accumulator has no drop path); drop variants use the classical evaluation. This
is a correctness guard, not an oversight -- a net loaded there would return
confidently wrong scores.

---

## 7. Process, concurrency and lifecycle

### 7.1 What holds

- **Poolable:** each worker is a plain process reading stdin, writing stdout.
  Safe to kill at any point; no shared files, no locks, no external state.
- **Fast startup:** validator startup is attack tables plus a 1 MB allocation.
- **Health probe:** `isready` -> `readyok`, on both tiers.
- **Timeouts:** every call is bounded by the Adapter. Validator calls are
  microseconds; a validator worker that stops answering is hung and should be
  evicted.

### 7.2 Concurrency model

**One request at a time per process.** The engine has process-global mutable
state (transposition table, move-ordering history), so a single process must
not serve concurrent positions. Use your preferred model from section 7 -- many
single-request worker processes. Do not multiplex requests onto one worker.

### 7.3 Protocol: line-based JSON, not gRPC

The engine speaks lines on stdin/stdout. The Adapter owns the gRPC boundary.

Concretely, the surface is a hybrid and you should know which half is which:

- **Setup commands are UCI-shaped text:** `setoption`, `position`, `isready`.
- **Validator queries return JSON:** `getfen`, `legalmoves`, `apply`, `status`,
  `canmate`.
- **Search returns UCI text:** `info ...` lines and `bestmove ...`.

So the Adapter still parses UCI text for the search tier. If you want the
search tier to return JSON too (`{"bestMove":...,"scoreCp":...,"pv":[...]}`),
that is a small addition -- say the word.

Each validator request is currently **two lines** (`position ...` then the
query). That is stateless in the sense that matters -- the worker holds no game
identity and any worker answers any request -- but it is not one line per
request. If the Adapter would rather send a single self-contained line, we can
add a combined form.

---

## 8. Non-goals

Agreed and unchanged. The engine knows nothing about networking, sockets,
Redis, players, accounts, client IDs, rooms, matchmaking, clocks, chat,
authentication or bans. It receives positions and returns chess answers.

---

## 9. Checklist status

| # | Requirement | Status |
|---|---|---|
| 1 | Lean validator mode, separable from search | Done -- `ChessEngine-validator` |
| 2 | `validateAndApply` -> legal + position + terminal + `capturedDropType` | Done -- `apply` |
| 3 | `legalMoves` -> exact UCI set | Done -- `legalmoves` |
| 4 | `canSideMate` | Done -- `canmate`, and valid for drop variants too |
| 5 | Bughouse per-board draw detection | Done -- **requires `DrawRules standard`** (3.3) |
| 6 | crazyhouse drops, in-position hand encoding | Done |
| 7 | bughouse drops, externally injected hand | Done -- **server owns all hand movement** (3.2) |
| 8 | Stateless, threefold via history, deterministic | Done -- move-list history; search determinism needs `Threads 1` (4.3) |
| 9 | Error taxonomy | Done -- `internal_error` reserved, not emitted (5) |
| 10 | Search API with budget, eval, PV/MultiPV | Done -- strength caveat (6) |
| 11 | Poolable workers | Done -- one request at a time per process (7.2) |
| 12 | Canonical wire format | Proposed, awaiting your confirmation (4.1) |

---

## 10. What we need from you

1. **Confirm the wire format** in 4.1, in particular that the `~` promoted
   marker survives persistence. Without it, promotion revert cannot work after
   a reload, and captured promoted pieces will wrongly re-enter reserves as
   queens.
2. **Confirm `DrawRules standard` for bughouse boards** (3.3), or tell us you
   want real bughouse rules and will not adjudicate those draws.
3. **Decide on threefold input** (4.2): is the move-list form enough, or do you
   store positions and need a position-list form?
4. **Tell us if search determinism matters** (4.3). If yes, we pin `Threads 1`
   for AI moves and accept the strength cost.
5. **Tell us the priority of drop-variant playing strength** (6). Legal-but-weak
   is fine for validation and probably not fine for a user-facing AI opponent.
6. **Confirm the protocol split is acceptable** (7.3), or ask for JSON search
   output and a single-line request form.
