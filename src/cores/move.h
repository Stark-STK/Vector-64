#ifndef MOVE_H
#define MOVE_H

#include "types.h"
#include <cassert>

namespace Core {

class Move {
public:
  enum Flag : int {
    QUIET = 0b0000,
    DOUBLE_PUSH = 0b0001,
    CASTLING = 0b0010,
    EN_PASSANT = 0b0011,
    CAPTURE = 0b0100,
    // Bughouse: a piece from the hand placed on an empty square. There is no
    // origin square, so the from_sq field carries the dropped PieceType
    // instead. Keeps Move at 16 bits, so the TT, PV and killer tables are
    // unaffected. 0b0110 and 0b0111 remain unused.
    DROP = 0b0101,
    PROMOTION_N = 0b1000,
    PROMOTION_B = 0b1001,
    PROMOTION_R = 0b1010,
    PROMOTION_Q = 0b1011,
    PROMOTION_CAP_N = 0b1100,
    PROMOTION_CAP_B = 0b1101,
    PROMOTION_CAP_R = 0b1110,
    PROMOTION_CAP_Q = 0b1111
  };

  Move() : data(0) {}
  Move(uint16_t d) : data(d) {}

  Move(Square from, Square to, Flag flag = QUIET) {
    data = static_cast<uint16_t>(from | (to << 6) | (flag << 12));
  }

  static Move make_quiet(Square from, Square to) {
    return Move(from, to, QUIET);
  }

  // Bughouse drop: `pt` rides in the from_sq field (see DROP).
  static Move make_drop(PieceType pt, Square to) {
    return Move(static_cast<Square>(pt), to, DROP);
  }

  static Move none() { return Move(0); }

  constexpr Square from_sq() const { return static_cast<Square>(data & 0x3F); }

  constexpr Square to_sq() const {
    return static_cast<Square>((data >> 6) & 0x3F);
  }

  constexpr int flags() const { return (data >> 12) & 0xF; }

#if defined(ENGINE_VARIANTS)
  // Exact per-flag capture mask rather than a bit test: DROP (0b0101) shares
  // bit 2 with CAPTURE, so `data & 0x4000` would misread every drop as a
  // capture. Bits set for EN_PASSANT, CAPTURE and the four PROMOTION_CAP_*.
  // The standard build keeps the original bit test byte for byte.
  static constexpr uint16_t CAPTURE_FLAGS = 0xF018;

  bool is_capture() const { return (CAPTURE_FLAGS >> flags()) & 1; }

  bool is_drop() const { return flags() == DROP; }
#else
  bool is_capture() const {
    return (data & 0x4000) != 0 || flags() == EN_PASSANT;
  }

  static constexpr bool is_drop() { return false; }
#endif

  bool is_promotion() const { return (data & 0x8000) != 0; }

  // Valid only when is_drop(); the from_sq field holds the piece type.
  PieceType dropped_piece() const {
    return static_cast<PieceType>(data & 0x3F);
  }

  bool is_en_passant() const { return flags() == EN_PASSANT; }

  bool is_castling() const { return flags() == CASTLING; }

  bool is_double_push() const { return flags() == DOUBLE_PUSH; }

  bool is_ok() const { return data != 0; }

  PieceType promotion_type() const {
    if (!is_promotion())
      return NO_PIECE_TYPE;
    int p = (data >> 12) & 3;
    switch (p) {
    case 0:
      return KNIGHT;
    case 1:
      return BISHOP;
    case 2:
      return ROOK;
    case 3:
      return QUEEN;
    }
    return NO_PIECE_TYPE;
  }

  bool operator==(const Move &other) const { return data == other.data; }
  bool operator!=(const Move &other) const { return data != other.data; }

  uint16_t raw() const { return data; }

private:
  uint16_t data;
};

struct MoveList {
#if defined(ENGINE_VARIANTS)
  // 256 covers standard chess (max 218 legal moves), but a drop-variant
  // position adds up to five piece types x every empty square on top of the
  // board moves, which can exceed it. Only the variant build pays the larger
  // stack frame; the standard build keeps 256.
  static constexpr int MAX_MOVES = 512;
#else
  static constexpr int MAX_MOVES = 256;
#endif

  Move moves[MAX_MOVES];
  int count;

  MoveList() : count(0) {}

  void push_back(Move m) {
    assert(count < MAX_MOVES);
    moves[count++] = m;
  }

  void clear() { count = 0; }

  int size() const { return count; }

  Move *begin() { return moves; }
  Move *end() { return moves + count; }
  const Move *begin() const { return moves; }
  const Move *end() const { return moves + count; }

  Move operator[](int index) const {
    assert(index >= 0 && index < count);
    return moves[index];
  }
};

} // namespace Core

#endif
