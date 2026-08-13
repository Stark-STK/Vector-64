#ifndef UCI_UTIL_H
#define UCI_UTIL_H

#include "../cores/move.h"
#include "../cores/types.h"

#include <string>

// Move formatting shared by the UCI loop and the visualizer (src/viz), which
// reports moves in the same long-algebraic notation the protocol uses.

namespace UCI {

inline char promo_to_char(Core::PieceType pt) {
  switch (pt) {
  case Core::KNIGHT:
    return 'n';
  case Core::BISHOP:
    return 'b';
  case Core::ROOK:
    return 'r';
  default: // queen (and any non-underpromotion) prints as 'q'
    return 'q';
  }
}

// Uppercase piece letter used by drop notation.
inline char drop_to_char(Core::PieceType pt) {
  switch (pt) {
  case Core::PAWN:
    return 'P';
  case Core::KNIGHT:
    return 'N';
  case Core::BISHOP:
    return 'B';
  case Core::ROOK:
    return 'R';
  case Core::QUEEN:
    return 'Q';
  default:
    return '?';
  }
}

// Long algebraic ("e2e4", "e7e8q"), or drop notation ("P@e4") in the drop
// variants; "0000" for a null/invalid move.
inline std::string move_to_uci(Core::Move m) {
  if (!m.is_ok())
    return "0000";
#if defined(ENGINE_VARIANTS)
  if (m.is_drop()) {
    char out[4];
    out[0] = drop_to_char(m.dropped_piece());
    out[1] = '@';
    out[2] = static_cast<char>('a' + Core::file_of(m.to_sq()));
    out[3] = static_cast<char>('1' + Core::rank_of(m.to_sq()));
    return std::string(out, out + 4);
  }
#endif
  char out[6] = {0, 0, 0, 0, 0, 0};
  out[0] = static_cast<char>('a' + Core::file_of(m.from_sq()));
  out[1] = static_cast<char>('1' + Core::rank_of(m.from_sq()));
  out[2] = static_cast<char>('a' + Core::file_of(m.to_sq()));
  out[3] = static_cast<char>('1' + Core::rank_of(m.to_sq()));
  if (m.is_promotion()) {
    out[4] = promo_to_char(m.promotion_type());
    return std::string(out, out + 5);
  }
  return std::string(out, out + 4);
}

} // namespace UCI

#endif
