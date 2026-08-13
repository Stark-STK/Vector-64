#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "types.h"

namespace Core {

// Largest hand count the hash distinguishes. A bughouse hand is bounded by
// the pieces in play across both boards; counts are clamped for hashing only,
// the counters themselves are exact.
constexpr int MAX_IN_HAND = 16;

namespace Zobrist {
using Key = uint64_t;
extern Key psq[COLOR_NB][PIECE_TYPE_NB][64];
extern Key enpassant[FILE_NB];
extern Key castling[16];
extern Key side;
#if defined(ENGINE_VARIANTS)
// Drop variants. Hands are part of position identity -- the same board with
// different reserves is a different position, and a TT that ignores this
// returns wrong scores. Hashing by count keeps each update to one
// XOR-out/XOR-in pair. `promotedSq` distinguishes a promoted piece, which
// returns to the capturer's hand as a pawn rather than as itself.
extern Key hand[COLOR_NB][PIECE_TYPE_NB][MAX_IN_HAND + 1];
extern Key promotedSq[64];
#endif

void init();

} // namespace Zobrist

} // namespace Core

#endif
