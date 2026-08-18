#!/usr/bin/env python3
"""Fixed-node engine-vs-engine testing: fixed matches and SPRT.

    # fixed-length match (Elo estimate):
    python tools/nnue/match.py --engine <exe> --net <big.nnue> --games 200

    # SPRT (the gate for search/eval changes):
    python tools/nnue/match.py --engine <exe> --net <big.nnue> \
        --base-engine <base-exe> --sprt 0 5 --concurrency 6

    # crazyhouse SPRT between two builds:
    python tools/nnue/match.py --variant crazyhouse \
        --engine <new-exe> --base-engine <base-exe> \
        --arbiter <ChessEngine-validator> --sprt 0 5

Both sides play color-swapped pairs on generated openings (seeded random
walks, material-balanced, unlimited variety -- essential because fixed-node
games are deterministic, so every (opening, color) yields exactly one
outcome and repeats add no information). Games run on parallel workers;
fixed nodes make results load-independent.

Adjudication comes from the engine's own validator binary, not a chess
library: house rule is that nothing outside the engine encodes chess rules,
so legality, move application and terminal detection all come from
ChessEngine-validator over its JSON surface. That is also what makes variant
testing possible at all -- the arbiter speaks crazyhouse and bughouse because
the engine does.

SPRT uses the trinomial GSPRT approximation with H0: elo <= elo0 and
H1: elo >= elo1, alpha = beta = 0.05; the run stops when the LLR crosses
+/- ln((1-beta)/alpha) ~= 2.944.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import queue
import random
import subprocess
import sys
import threading
from dataclasses import dataclass

LLR_BOUND = math.log((1 - 0.05) / 0.05)  # ~2.944 for alpha = beta = 0.05

# Centipawn values for the opening-balance filter. Counting letters in a FEN
# board field is string handling, not chess logic, so this stays local.
PIECE_VALUES = {"p": 100, "n": 320, "b": 330, "r": 500, "q": 900}


def material_imbalance(fen: str) -> int:
    """White-minus-black material, read straight off a FEN board field."""
    board = fen.split()[0].split("[")[0]
    total = 0
    for ch in board:
        value = PIECE_VALUES.get(ch.lower())
        if value is not None:
            total += value if ch.isupper() else -value
    return total


class Arbiter:
    """Game rules, supplied by the engine's validator binary.

    Every query is a single self-contained line carrying its own position, so
    the arbiter holds no game state and one instance can serve a whole worker.
    """

    def __init__(self, binary: str, variant: str = "chess"):
        self.proc = subprocess.Popen(
            [os.path.abspath(binary)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, text=True, bufsize=1
        )
        self._send("uci")
        self._wait_token("uciok")
        if variant != "chess":
            self._send(f"setoption name UCI_Variant value {variant}")
        self._send("isready")
        self._wait_token("readyok")

    def _send(self, s: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(s + "\n")
        self.proc.stdin.flush()

    def _wait_token(self, token: str) -> None:
        assert self.proc.stdout is not None
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("arbiter died")
            if line.startswith(token):
                return

    def _query(self, command: str) -> dict:
        """Send one command and read back its single JSON reply line."""
        assert self.proc.stdout is not None
        self._send(command)
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("arbiter died")
            line = line.strip()
            if line.startswith("{"):
                return json.loads(line)
            # `info string` chatter is not a reply; keep reading.

    @staticmethod
    def _clause(moves: list[str]) -> str:
        return "startpos" + (" moves " + " ".join(moves) if moves else "")

    def status(self, moves: list[str]) -> dict:
        return self._query(f"status {self._clause(moves)}")

    def legal_moves(self, moves: list[str]) -> list[str]:
        return self._query(f"legalmoves {self._clause(moves)}")["moves"]

    def children(self, moves: list[str]) -> list[dict]:
        """Every legal move plus the position it reaches, in one round trip.

        Each entry has move, fen, sideToMove, inCheck and terminal -- a whole
        ply of lookahead without a query per move.
        """
        return self._query(f"children {self._clause(moves)}")["children"]

    def fen(self, moves: list[str]) -> str:
        return self._query(f"getfen {self._clause(moves)}")["fen"]

    def apply(self, moves: list[str], move: str) -> dict:
        """Validate `move` in the position after `moves`; returns the reply."""
        return self._query(f"apply {move} {self._clause(moves)}")

    def quit(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def generate_openings(arbiter: Arbiter, count: int, seed: int,
                      plies: int = 8) -> list[str]:
    """Seeded random legal walks, filtered to quiet, material-balanced ends."""
    rng = random.Random(seed)
    out: list[str] = []
    while len(out) < count:
        moves: list[str] = []
        ok = True
        for _ in range(plies):
            legal = arbiter.legal_moves(moves)
            if not legal:
                ok = False
                break
            moves.append(legal[rng.randrange(len(legal))])
        if not ok:
            continue
        state = arbiter.status(moves)
        if state["terminal"] != "none" or state["inCheck"]:
            continue
        if abs(material_imbalance(arbiter.fen(moves))) > 150:
            continue
        out.append(" ".join(moves))
    return out


class Engine:
    def __init__(self, binary: str, net: str | None, small: str | None = None,
                 options: str | None = None, variant: str = "chess"):
        self.proc = subprocess.Popen(
            [os.path.abspath(binary)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, text=True, bufsize=1
        )
        self._send("uci")
        self._wait("uciok")
        # Variant first: it resets the position, so a later `position` command
        # must not be overwritten by it.
        if variant != "chess":
            self._send(f"setoption name UCI_Variant value {variant}")
        self._send("setoption name Threads value 1")
        self._send("setoption name Hash value 64")
        if net:
            self._send(f"setoption name EvalFile value {net}")
        if small:
            self._send(f"setoption name EvalFileSmall value {small}")
        for opt in (options or "").split(";"):
            if "=" in opt:
                name, value = opt.split("=", 1)
                self._send(f"setoption name {name.strip()} value {value.strip()}")
        self._send("isready")
        self._wait("readyok")

    def _send(self, s: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(s + "\n")
        self.proc.stdin.flush()

    def _wait(self, token: str) -> str:
        assert self.proc.stdout is not None
        out: list[str] = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine died:\n" + "".join(out[-10:]))
            out.append(line)
            if line.startswith(token):
                return line.strip()

    def new_game(self) -> None:
        self._send("ucinewgame")
        self._send("isready")
        self._wait("readyok")

    def best_move(self, moves: list[str], nodes: int) -> str:
        pos = "position startpos" + (" moves " + " ".join(moves) if moves else "")
        self._send(pos)
        self._send(f"go nodes {nodes}")
        return self._wait("bestmove").split()[1]

    def quit(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


@dataclass
class Tally:
    wins: int = 0
    draws: int = 0
    losses: int = 0

    @property
    def n(self) -> int:
        return self.wins + self.draws + self.losses

    def score(self) -> float:
        return (self.wins + 0.5 * self.draws) / max(self.n, 1)

    def elo(self) -> tuple[float, float]:
        """Elo difference and 95% half-interval from the score fraction."""
        n = max(self.n, 1)
        p = min(max(self.score(), 1e-6), 1 - 1e-6)
        elo = -400.0 * math.log10(1.0 / p - 1.0)
        var = (self.wins * (1 - p) ** 2 + self.draws * (0.5 - p) ** 2 +
               self.losses * p**2) / n
        se = math.sqrt(var / n)
        lo, hi = p - 1.96 * se, p + 1.96 * se
        half = 0.0
        if 0 < lo and hi < 1:
            e_lo = -400.0 * math.log10(1.0 / lo - 1.0)
            e_hi = -400.0 * math.log10(1.0 / hi - 1.0)
            half = (e_hi - e_lo) / 2.0
        return elo, half

    def llr(self, elo0: float, elo1: float) -> float:
        """Trinomial GSPRT approximation of the log-likelihood ratio."""
        n = self.n
        if n == 0 or self.wins == n or self.losses == n or self.draws == n:
            return 0.0
        s = self.score()
        m2 = (self.wins + 0.25 * self.draws) / n
        var = m2 - s * s
        if var <= 0:
            return 0.0
        s0 = 1.0 / (1.0 + 10.0 ** (-elo0 / 400.0))
        s1 = 1.0 / (1.0 + 10.0 ** (-elo1 / 400.0))
        return (s1 - s0) * (2 * s - s0 - s1) / (2 * var / n)


def play_game(white: Engine, black: Engine, arbiter: Arbiter, opening: str,
              nodes: int, max_plies: int) -> str:
    """Returns "1-0", "0-1" or "1/2-1/2". The arbiter decides everything."""
    moves = opening.split()
    white.new_game()
    black.new_game()

    state = arbiter.status(moves)
    while True:
        terminal = state["terminal"]
        if terminal == "checkmate":
            # The side to move is the one that got mated.
            return "0-1" if state["sideToMove"] == "w" else "1-0"
        if terminal != "none":
            return "1/2-1/2"
        if len(moves) >= max_plies:
            return "1/2-1/2"

        to_move = state["sideToMove"]
        engine = white if to_move == "w" else black
        move = engine.best_move(moves, nodes)

        # `apply` both validates and advances, so an illegal engine move is a
        # forfeit rather than a crash.
        reply = arbiter.apply(moves, move)
        if not reply.get("ok"):
            return "0-1" if to_move == "w" else "1-0"
        moves.append(move)
        state = reply


def main() -> int:
    p = argparse.ArgumentParser(description="Fixed-node match / SPRT runner.")
    p.add_argument("--engine", required=True, help="binary for the 'new' side")
    p.add_argument("--base-engine", default=None,
                   help="binary for the base side (default: same as --engine)")
    p.add_argument("--arbiter", default=None,
                   help="validator binary used as the referee "
                        "(default: ChessEngine-validator beside --engine)")
    p.add_argument("--variant", default="chess",
                   choices=["chess", "crazyhouse", "bughouse"],
                   help="variant for both engines and the arbiter")
    p.add_argument("--net", default=None, help="EvalFile for the 'new' side")
    p.add_argument("--net-small", default=None,
                   help="EvalFileSmall for the 'new' side (dual-net)")
    p.add_argument("--base-net", default=None,
                   help="EvalFile for the base side (default: classical)")
    p.add_argument("--base-net-small", default=None)
    p.add_argument("--uci-new", default=None,
                   help='extra options for the new side, "Name=Val;Name=Val"')
    p.add_argument("--uci-base", default=None,
                   help="extra options for the base side")
    p.add_argument("--games", type=int, default=200,
                   help="game cap (SPRT may stop earlier)")
    p.add_argument("--nodes", type=int, default=8000)
    p.add_argument("--max-plies", type=int, default=400)
    p.add_argument("--concurrency", type=int, default=6)
    p.add_argument("--seed", type=int, default=2024,
                   help="opening-book seed (fixed seed = reproducible run)")
    p.add_argument("--sprt", nargs=2, type=float, metavar=("ELO0", "ELO1"),
                   default=None, help="run as SPRT with H0 elo<=ELO0, H1 elo>=ELO1")
    args = p.parse_args()

    arbiter_bin = args.arbiter
    if arbiter_bin is None:
        directory = os.path.dirname(os.path.abspath(args.engine))
        name = "ChessEngine-validator"
        if sys.platform == "win32":
            name += ".exe"
        arbiter_bin = os.path.join(directory, name)
    if not os.path.exists(arbiter_bin):
        print(f"arbiter not found: {arbiter_bin}\n"
              f"Build it: cmake --build <builddir> --target ChessEngine-validator",
              file=sys.stderr)
        return 3

    book_arbiter = Arbiter(arbiter_bin, args.variant)
    try:
        pair_count = (args.games + 1) // 2
        openings = generate_openings(book_arbiter, pair_count, args.seed)
    finally:
        book_arbiter.quit()

    jobs: queue.Queue[str | None] = queue.Queue()
    for op in openings:
        jobs.put(op)
    for _ in range(args.concurrency):
        jobs.put(None)

    tally = Tally()
    lock = threading.Lock()
    stop = threading.Event()

    def report(final: bool = False) -> None:
        elo, half = tally.elo()
        line = (f"games {tally.n:4d}  +{tally.wins} ={tally.draws} "
                f"-{tally.losses}  elo {elo:+7.1f} +/- {half:5.1f}")
        if args.sprt:
            line += (f"  LLR {tally.llr(*args.sprt):+6.2f} "
                     f"[{-LLR_BOUND:.2f}, {LLR_BOUND:.2f}]")
        print(("final: " if final else "") + line, flush=True)

    def worker() -> None:
        new_eng = Engine(args.engine, args.net, args.net_small, args.uci_new,
                         args.variant)
        base_eng = Engine(args.base_engine or args.engine, args.base_net,
                          args.base_net_small, args.uci_base, args.variant)
        referee = Arbiter(arbiter_bin, args.variant)
        try:
            while not stop.is_set():
                op = jobs.get()
                if op is None:
                    break
                for new_is_white in (True, False):
                    if stop.is_set():
                        break
                    white, black = ((new_eng, base_eng) if new_is_white
                                    else (base_eng, new_eng))
                    result = play_game(white, black, referee, op, args.nodes,
                                       args.max_plies)
                    with lock:
                        if result == "1/2-1/2":
                            tally.draws += 1
                        elif (result == "1-0") == new_is_white:
                            tally.wins += 1
                        else:
                            tally.losses += 1
                        if tally.n % 10 == 0:
                            report()
                        if args.sprt and tally.n >= 20 and not stop.is_set():
                            llr = tally.llr(*args.sprt)
                            if llr >= LLR_BOUND:
                                print("SPRT: H1 accepted (change is a gain)",
                                      flush=True)
                                stop.set()
                            elif llr <= -LLR_BOUND:
                                print("SPRT: H0 accepted (no gain / a loss)",
                                      flush=True)
                                stop.set()
        finally:
            new_eng.quit()
            base_eng.quit()
            referee.quit()

    threads = [threading.Thread(target=worker) for _ in range(args.concurrency)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    report(final=True)
    if args.sprt:
        llr = tally.llr(*args.sprt)
        if llr >= LLR_BOUND:
            return 0
        if llr <= -LLR_BOUND:
            return 1
        # The bound may have been crossed mid-run and then drifted back while
        # the in-flight games of the other workers drained. That is still a
        # decision -- the stop fired -- so report it as one instead of
        # contradicting the "H1 accepted" line already printed.
        if stop.is_set():
            print(f"SPRT: bound was crossed during the run "
                  f"(final LLR {llr:+.2f} after in-flight games drained)",
                  flush=True)
            return 0
        print("SPRT: inconclusive at game cap", flush=True)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
