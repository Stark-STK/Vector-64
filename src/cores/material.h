#ifndef CORES_MATERIAL_H
#define CORES_MATERIAL_H

#include "bitboard.h"
#include "position.h"
#include "types.h"

namespace Core {

// Whether `c` still holds material with which checkmate is reachable by some
// legal continuation -- the FIDE 6.9 sense used to adjudicate a flag, not
// "can force mate". A lone king, king + knight and king + bishop cannot
// deliver mate at all; two minors can (K+N+N mates only with cooperation, but
// a helpmate exists, so it counts as sufficient). A pawn counts because it
// promotes.
//
// In a drop variant anything in hand is enough on its own: a reserve piece can
// be dropped to build a mating net regardless of what stands on the board.
inline bool can_side_mate(const Position &pos, Color c) {
  if (pos.pieces(PAWN, c) || pos.pieces(ROOK, c) || pos.pieces(QUEEN, c))
    return true;
  if (pos.has_any_in_hand(c))
    return true;
  const Bitboard minors = pos.pieces(KNIGHT, c) | pos.pieces(BISHOP, c);
  return popcount(minors) >= 2;
}

// The position is drawn because neither side can ever deliver mate.
inline bool insufficient_material_both(const Position &pos) {
  return !can_side_mate(pos, WHITE) && !can_side_mate(pos, BLACK);
}

} // namespace Core

#endif
