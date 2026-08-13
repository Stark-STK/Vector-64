# STK-Vector-64 Variant Support

Status: Crazyhouse and bughouse board mechanics and the host integration
surface implemented; search and evaluation tuning outstanding (section 13).

Shipped as a separate binary, `ChessEngine-variants`, selected at runtime with
`setoption name UCI_Variant value crazyhouse|bughouse`.

Related: `docs/architecture.md` (current implementation), `docs/vector64-spec.md`
(production profile).

## 1. Purpose

This document specifies how variant play is added to STK-Vector-64. It defines
the staging order, the move-encoding and position-state changes drops require,
which search techniques stay valid and which must be retuned or disabled, the
NNUE feature-set extension, and the module boundary that keeps multi-variant
work off the standard engine's hot path.

## 2. Prime Directive

The standard engine is the product. Every variant change is subject to:

- **Bench signature is unchanged.** `ChessEngine` bench 5935930 and
  `ChessEngine-nnue` bench 2704705 must hold before and after any variant
  commit. A changed bench is a bug, not a rebaseline.
- **No NPS regression.** Variant state may not be tested in the standard
  make/unmake, movegen, or accumulator-update paths.
- **`cores/` stays 64-square and two-colour.** Geometries that break either
  assumption live in a separate module (section 9, Tier B).

These are enforced by construction rather than by measurement: every line of
variant code sits behind `#if defined(ENGINE_VARIANTS)` and compiles into a
*separate* core library (section 9). The standard binary therefore contains no
reserve state, no branches in `move_piece`/`remove_piece`, and a 256-entry
`MoveList` exactly as before.

## 3. Staging

Bughouse is not one project. It decomposes into a search problem and a
partner-coordination problem, and the search problem is *crazyhouse*.

| Stage | Deliverable | Nature of the work | State |
|---|---|---|---|
| 1 | Drop mechanics | Reserves, drops, promoted tracking, hashing, FEN, UCI | **Done** |
| 2 | Search + eval tuning | Retuned pruning, drop-aware SEE, drop-checks in qsearch | Outstanding |
| 3 | Bughouse partner layer | Protocol, incoming-material model, sitting | Outstanding |
| 4 | Variant NNUE | Hand-feature block, self-play net | Outstanding |
| 5 | Tier B variants | Separate module, new geometry, new search paradigm | Not planned |

Crazyhouse and bughouse share every board mechanic, so stage 1 delivered both
at once: they differ only in where the reserve comes from and in the draw
rules. Crazyhouse is a complete, shippable variant on its own -- Lichess
supports it, it speaks UCI with `UCI_Variant`, and a bot can play it today.

Stage 2 adds almost no search code. A bughouse board *is* a crazyhouse board;
what differs is that the hand is supplied stochastically by the partner board,
and that time and sitting become first-class strategic resources. Those are
policy layers above the search, and they are where bughouse stops being a pure
engine problem.

## 4. Move Encoding

`Core::Move` (`src/cores/move.h`) packs `from | to << 6 | flag << 12` into a
single `uint16_t`. The field layout is full, but the flag space is not: `QUIET`
through `CAPTURE` use `0b0000`-`0b0100` and promotions use `0b1000`-`0b1111`,
leaving **`0b0101`, `0b0110`, `0b0111` unused**.

A drop needs a destination square and a dropped piece type (pawn, knight,
bishop, rook, queen -- five values, three bits). It has no origin square. So:

```
DROP = 0b0101
  bits 0-5   : dropped PieceType (reusing the from_sq field)
  bits 6-11  : destination square
  bits 12-15 : DROP
```

This keeps `Move` at 16 bits, so the transposition table, PV array, killer
tables, and every `Move`-sized buffer in the engine are untouched. `Move::none()`
is `data == 0` and a drop always has a nonzero flag nibble, so the
none-move sentinel stays sound.

Required audit -- every site that interprets `from_sq()` as a square:

- `MoveOrdering` and continuation history, keyed on `(piece, to)`. Drops supply
  the piece type directly, so the key is well-defined; the *lookup* is fine, the
  code path that derives the piece from `piece_on(from_sq())` is not.
- `is_pseudo_legal()` (`src/cores/movegen.h:72`) -- see section 6.
- NNUE incremental update, which reads the moving piece off the origin square.
- `move_to_uci()` (`src/uci/uci_util.h`), which unconditionally formats four
  squares.

UCI notation follows the crazyhouse convention: `P@e4`, `N@f7`. Uppercase piece
letter, `@`, destination. The parser and formatter both need the case.

## 5. Position State

### 5.1 Hands

```cpp
uint8_t hand[COLOR_NB][PIECE_TYPE_NB];   // PAWN..QUEEN used; KING never
```

### 5.2 Promoted-piece tracking

A promoted piece that is captured returns to the capturer's hand **as a pawn**,
not as the piece it had become. This is the rule most easily missed and it is
worth material every game. It requires one bitboard:

```cpp
Bitboard promoted;   // squares holding a piece that was promoted
```

Set on promotion, cleared on capture and on move-out, carried across
`move_piece`. `UndoInfo` gains a `capturedWasPromoted` flag; the hand deltas
themselves are derivable from the move plus `capturedPiece`, so hands do not
need to be stored wholesale in the undo record.

### 5.3 Zobrist

Hands are part of position identity -- the same board with different hands is a
different position, and a TT that ignores this will return catastrophically
wrong scores. `src/cores/zobrist.h` gains:

```cpp
extern Key hand[COLOR_NB][PIECE_TYPE_NB][MAX_IN_HAND + 1];
extern Key promotedSq[64];
```

Hashing hands by *count* (rather than XOR-ing one key per piece) keeps the
update to a single XOR-out/XOR-in pair per drop or capture.

`key_after()` (`src/cores/position.h:60`), which the search uses to prefetch the
child TT bucket, must be extended in lockstep -- its contract is that it equals
`hash()` after `make_move`, and the invariant check should assert that for drops
too.

### 5.4 FEN

Crazyhouse FEN appends the hand to the board field in brackets:

```
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[] w KQkq - 0 1
```

with e.g. `[QPpp]` for a white queen and pawn plus two black pawns. Promoted
pieces are conventionally marked with a trailing `~` on the board field.
`setFromFEN`/`toFEN` handle both; the standard parser must reject the bracket
form so a crazyhouse FEN cannot be silently mis-loaded into a standard search.

### 5.5 Draw rules

`is_repetition()` (`src/cores/position.cpp:549`) keys on the Zobrist hash, so
once hands are hashed it is *correct for crazyhouse* with no further change.

For bughouse it must be **disabled entirely** -- a repeated position on your
board is not a repeated game state, because the other board has moved on. Same
for the fifty-move rule. This is a variant trait, not a code change.

## 6. Move Generation

### 6.1 Drop generation

For each piece type with a nonzero hand count, for each empty square; pawns
excluded from ranks 1 and 8. There are no further restrictions -- unlike shogi,
crazyhouse and bughouse permit drop-mate and dropping a pawn on a file that
already has one.

### 6.2 Drop legality is cheap

A drop can never expose your own king, since it only ever *adds* a blocker.
Therefore:

- Not in check -> every drop is legal, no test needed.
- Single check by a slider -> legal drop targets are exactly the squares between
  king and checker (`Attacks::between_bb`).
- Single check by a knight or pawn, or any double check -> **no drop is legal.**

This collapses to a single precomputed mask ANDed against the empty squares,
and it fits `NodeLegality` (`src/cores/movegen.h:14`) naturally.

Implementing it surfaced a **pre-existing bug in `compute_between`**
(`src/cores/attacks.cpp`). It normalized the file/rank deltas to -1/0/1 and
*then* tested `abs(df) != abs(dr)` to reject non-aligned square pairs -- a test
that can never fire once both values are unit steps. `Between[s1][s2]` for
non-aligned squares therefore held a diagonal ray walking off the edge of the
board instead of the empty set. `compute_line` had the identical defect.

Nothing in standard chess noticed, because every existing caller passes
aligned squares by construction (`ensure_pins` derives its snipers from slider
attacks radiating out of the king square). Drop legality is the first caller
that can pass a non-aligned pair -- king versus a knight or pawn checker -- and
it read `between_bb` as "no square blocks this check". The garbage ray made
knight checks appear blockable, so a drop could answer a knight check, leaving
the king en prise and the opponent free to capture it a ply later.

Both functions now test alignment on the raw deltas before normalizing. The
tables are only consulted for aligned pairs by existing code, so the standard
engine's behaviour and bench signature are unaffected -- verified.

### 6.3 MoveList overflow -- a real risk

`MoveList::MAX_MOVES` is 256 (`src/cores/move.h:89`). A crazyhouse position with
a full hand generates board moves *plus* up to five piece types x ~40 empty
squares. Documented crazyhouse positions exceed 256 legal moves. **Raise to 512
for variant builds** (via the variant traits, so the standard build keeps its
smaller, cache-friendlier list) and convert the `assert` in `push_back` into a
hard check under variant builds.

### 6.4 Pseudo-legality

`is_pseudo_legal()` exists so a hash collision or torn TT entry can never
corrupt `make_move`. Drops raise the stakes: an unvalidated drop decrements a
hand counter that may be zero, corrupting position state permanently rather than
just producing a bad move. Drop validation must check hand count > 0, target
square empty, and pawn-rank legality.

## 7. Search

Each crazyhouse board -- and each bughouse board -- is a two-player zero-sum
game. **Negamax and alpha-beta remain valid in full.** This is the entire reason
bughouse is the cheap variant. What needs attention is the pruning heuristics,
all of which encode assumptions about *chess* that crazyhouse violates.

| Technique | Location | Verdict |
|---|---|---|
| Alpha-beta, TT, iterative deepening | throughout | Valid unchanged |
| Mate scores | `search.cpp:467` | Valid unchanged |
| Null-move pruning | `search.cpp:653` | **Disable initially** |
| Reverse futility | `search.cpp:637` | Retune margins |
| LMR | `search.cpp:770` | Retune; do not reduce drop-checks |
| SEE / MVV-LVA | `move_ordering.cpp` | **Semantically broken** |
| Quiescence | `search.cpp:857` | **Must include drop-checks** |

Three of these deserve elaboration.

**SEE is wrong, not just mistuned.** A capture hands the captured piece to the
opponent's reserve. Winning a rook for a knight can lose on the spot if the
knight in their hand mates you. Static exchange evaluation must charge the
capturer for the material it donates. Until that is modelled, capture ordering
should fall back to a crazyhouse-specific table rather than reusing MVV-LVA
unchanged.

**Quiescence determines playing strength here.** Crazyhouse tactics are drop
sequences, not capture sequences. A captures-only qsearch will hang forced mates
constantly and no amount of eval quality compensates. Quiescence must generate
checking drops, which means a bounded, ordered subset -- checking drops adjacent
to or on lines to the enemy king -- or the qsearch explodes.

**Null move is the standard crazyhouse footgun.** Attacks develop so fast, and
tempo is so valuable, that "if I pass and I'm still winning, I'm winning" fails
routinely. Ship with it off; re-enable behind an SPRT gate, restricted to
positions where the opponent's hand is empty.

Every one of these is an SPRT question, not an opinion. The existing self-play
and SPRT tooling applies unchanged, since the game is still scalar and zero-sum.

## 8. Evaluation

### 8.1 Interim classical

Crazyhouse piece values compress sharply and pawns in hand are strong.
`PieceValue` (`src/cores/types.h:88`) needs a variant-specific table, plus a
term for material in hand and king-safety weighting that scales with the
*opponent's* hand.

### 8.2 NNUE

STK-HalfKA (`src/nnue/halfka.h`) survives structurally -- 64 squares, two
colours, one king anchor per perspective, scalar stm-relative output. It needs
one addition: a hand block appended to the 22528 board features.

```
hand feature = (side_relative, piece_type, count_bucket)
             = 2 x 5 x B  additional features per perspective
```

with counts bucketed (0, 1, 2, 3+) rather than one-hot to bound the block.
Incremental update on a drop is one board-feature add plus one hand-feature
swap -- the existing accumulator machinery handles both without structural
change. Promoted status needs no feature; a promoted queen evaluates as a queen,
and the distinction only matters at capture time.

The important consequence: **the training pipeline is reusable as-is.** The
target is still a single stm-relative scalar, so datagen, the bullet port, and
the Vector Scope visualizer all work with a feature-set swap rather than a
rewrite. There is no crazyhouse corpus to warm-start from, so the net trains
from self-play -- but that is a compute cost, not an engineering one.

## 9. Multi-Variant Architecture

The roadmap goal is "support all variants." The design constraint is that a
generic engine is a slower engine, and the standard engine is the product. The
resolution is two tiers with a hard boundary between them.

### Tier A -- 64 squares, two players

Crazyhouse and bughouse today; chess960, atomic, horde, king-of-the-hill,
three-check and racing kings would join on the same terms.

All reuse `cores/` wholesale, differing in a movegen delta, terminal-condition
predicates, and evaluation. The mechanism is a single build flag:

- `option(ENGINE_VARIANTS)` compiles `${CORE_SOURCES}` a second time into
  `chess_core_variants` with `-DENGINE_VARIANTS`, and links the extra binary
  `ChessEngine-variants` against it.
- `chess_core` and the two tournament binaries are built from the same sources
  *without* the define, so every variant construct -- the `Variant` enum member,
  the reserve arrays, the promoted bitboard, the `DROP` handling in
  make/unmake, the drop generator, the wider `MoveList` -- is preprocessed away.
- Within a variant build, the specific variant is a runtime field on
  `Position`, because crazyhouse and bughouse differ only in draw rules, not in
  any hot-path mechanic.

Compiling the core twice costs build time and nothing else, and it makes the
prime directive structural: the standard binary cannot regress from variant
work, because it never sees it.

### Tier B -- different geometry or player count

Four-player chess (14x14, 160 squares), chaturaji (8x8, four armies).

These break the two invariants `cores/` is built on. `Bitboard` is `uint64_t`
and every magic table, shift mask, and the `sq ^ 56` / `sq ^ 7` NNUE orientation
depends on 64 squares; `Color` is `{WHITE, BLACK}` with `operator~` as `c ^ 1`.
More fundamentally, four independent players are not zero-sum, so negamax and
alpha-beta do not generalize -- the options are max^n (correct, prunes almost
nothing) or paranoid search (treats all opponents as a coalition, keeps
alpha-beta, plays pessimistically).

**Tier B is a sibling module with its own binary.** Do not template `Position`
over board size and colour count to accommodate it. That refactor puts the
standard engine's NPS at risk in order to serve a variant nobody can distribute --
4PC has no engine protocol and no bot platform. Chaturaji is the better Tier B
entry point if the goal is the interesting problem, because 8x8 preserves the
entire bitboard and attack layer and isolates the work to search.

## 10. Bughouse Layer (Stage 2)

Once crazyhouse works, bughouse adds three things, none of them search.

**Protocol.** UCI has no bughouse. FICS's is the only established one. The
minimal extension is a `partner` command carrying the partner board's FEN and
both clocks, alongside the normal `position` for our own board. This should be
specified in this repo and documented, since nothing standard exists to conform
to.

**Incoming-material model.** The partner board tells us which pieces are likely
to arrive in our hand and which will arrive in our opponent's. v1 should use a
static model -- count what our partner is likely to capture -- rather than
searching the partner board. Searching both boards jointly is a research
project; searching one board with a good supply prior is a strong bot.

**Time and sitting.** Refusing to move while waiting for a piece is a core
bughouse skill and it is a time-management policy, not a search result. Out of
scope for v1; the engine plays its own board well and moves promptly.

## 11. Testing

`tests/cpp/test_variants.cpp`, registered as the ctest case
`variants.drops_state_and_perft` and therefore already covered by the CI
matrix's `ctest` step. It runs four checks:

- **Perft anchors, no external table needed.** From the start position no
  reserve can be non-empty until a capture has happened, so crazyhouse perft
  must equal standard chess *exactly* through depth 4 and must exceed it at
  depth 5, where the first drops appear. Measured: depths 1-4 match, and
  depth 5 gives 4888832 against chess's 4865609 -- 23223 extra drop
  continuations. (4888832 is also the published crazyhouse figure, so this
  agrees with the external reference as well.)
- **State consistency over random games.** 150 crazyhouse and 150 bughouse
  games verify after every ply that the incremental hash, reserves,
  promoted-piece mask, mailbox, material and psqt all match a from-scratch
  rebuild via `toFEN`/`setFromFEN`; that `key_after()` equals the real
  post-move hash; and that `unmake_move` restores state exactly. The run
  asserts it actually played drops, so it cannot pass vacuously.
- **Movegen equivalence and hardening.** The staged generators plus the fast
  legality path must reproduce `generate_legal_moves` exactly, and
  `is_pseudo_legal` is fuzzed with random 16-bit words to confirm it never
  accepts a move the generators do not produce -- a false accept on a drop
  would decrement a reserve counter that may be zero.
- **Isolation.** A standard-mode `Position` must reject a bracketed variant
  FEN rather than silently loading the board and discarding the reserves.

The standard gates are unchanged and still pass: both bench signatures, the
six-position perft suite, and `consistency.make_unmake_and_movegen`.

## 12. Host Integration Surface

A host that treats the engine as the single source of chess truth needs more
than UCI: it must be able to read a position back, enumerate legal moves, and
get structured terminal and error information. That surface lives in
`src/uci/validator.h` and is available in any `ENGINE_VARIANTS` build.

Every reply is one line of compact JSON, hand-built so `cores/` gains no
third-party dependency. The position is whatever the preceding `position`
command established, so a worker holds no game identity and any worker can
answer any request.

| Command | Reply |
|---|---|
| `getfen` | `{"ok":true,"fen":...,"variant":...,"drawRules":...}` |
| `legalmoves` | `{"ok":true,"count":N,"moves":[...]}` -- exact UCI, promotions and drops enumerated |
| `apply <uci>` | `{"ok":true,"legal":true,"fen":...,"sideToMove":...,"inCheck":...,"terminal":...,"capturedDropType":...}` |
| `status` | `{"ok":true,"sideToMove":...,"inCheck":...,"terminal":...,"legalCount":N,"repetitions":k}` |
| `canmate <w\|b>` | `{"ok":true,"side":...,"canMate":...}` |

`terminal` is one of `none`, `checkmate`, `stalemate`, `draw_fifty_move`,
`draw_threefold`, `draw_insufficient_material`.

Failures carry a fixed taxonomy the host can branch on, never collapsed into
one signal: `illegal_move` (well-formed, not legal here), `malformed_request`
(unparseable move or argument), `internal_error` (engine fault; evict the
worker).

### 12.1 Two tiers

`ChessEngine-validator` is the lean tier: one thread, a 1 MB table, no net, and
`go`/`bench` refused outright so a validator can never queue behind a search.
`ChessEngine-variants --validator` gives the same mode from the full binary.
Search requests go to `ChessEngine-variants` in its normal mode.

`isready`/`readyok` is the health probe. Startup allocates a 1 MB table and the
attack tables, nothing else.

### 12.2 Reserves are the host's to move

This is the one place the engine deliberately does *less* than crazyhouse.

- **Crazyhouse:** a capture enters the capturer's own reserve. The engine does
  this itself.
- **Bughouse:** a capture enters the **partner's** reserve on the *other*
  board, which this engine cannot see. So a bughouse capture adds nothing to
  either reserve here; the engine reports `capturedDropType` and the host
  injects it into the partner board's position. `Position::captures_fill_hand()`
  is the switch, set from the variant.

A bughouse position therefore only gains material through an injected FEN,
which is exactly the `[...]` reserve field. There is no separate hand-injection
call because the position already carries it.

### 12.3 Draw rules

Real bughouse has neither a repetition nor a fifty-move draw, because the
partner board keeps the game state moving. A host that adjudicates draws per
board may want them anyway, so the policy is selectable independently of the
variant: `setoption name DrawRules value variant|standard`. `variant` (the
default) gives each variant its own rules; `standard` forces threefold and
fifty-move on for every variant.

`repetition_count()` reports occurrences within the fifty-move window, so the
host gets true threefold (count >= 2) rather than the twofold convention the
search uses internally. Repetition is detected from the moves supplied on the
`position ... moves ...` command, so the host passes history as a move list.

### 12.4 Determinism

Identical `(position, moves, variant, draw rules)` always yields an identical
validator result -- none of that surface searches. **Search** results are
deterministic only at `Threads 1`; lazy SMP is non-deterministic by design.

## 13. Remaining Work

The board mechanics are correct and the engine plays legal crazyhouse and
bughouse. It does not yet play them *well*. In rough priority order:

1. **Drop-checks in quiescence.** Drops are generated as quiet moves, so
   `generate_legal_captures` -- and therefore qsearch -- never sees them. In a
   drop variant the main tactical motif is a checking drop, so the engine will
   hang forced mates until qsearch generates a bounded, ordered subset of
   checking drops. This is the single largest strength gap.
2. **Drop-aware SEE and capture ordering.** A capture donates the captured
   piece to the opponent's reserve, so MVV-LVA and SEE are not merely
   mistuned but semantically wrong (section 7).
3. **Pruning retune.** Null move, reverse futility and LMR margins all encode
   chess assumptions that drop variants violate. Each is an SPRT question.
4. **Evaluation.** `PieceValue` needs a variant table plus terms for material
   in hand and for king safety scaled by the *opponent's* reserve.
5. **NNUE.** The variant binary currently has no net trained for drops. A net
   without the hand-feature block would evaluate reserves as invisible; the
   accumulator update also has no drop path yet, so a net must not be loaded
   in variant mode until both land (section 8.2).
6. **Bughouse partner layer.** Protocol, incoming-material model, sitting
   (section 10). Until this exists, bughouse mode is crazyhouse with the repetition
   and fifty-move draws switched off.

## 14. Open Questions

- Does drop-check quiescence need its own depth limit, or does the existing
  quiescence depth cap suffice once the drop subset is bounded?
- Hand count bucketing for NNUE: is 3+ enough resolution, or does crazyhouse
  need exact counts up to 5?
- Is a crazyhouse-tuned SEE worth building, or is a static capture table plus
  deeper search the better return?
- For bughouse: is the partner board worth any search at all, or is a static
  supply prior the right permanent answer?
