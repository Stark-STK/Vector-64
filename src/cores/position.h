#ifndef POSITION_H
#define POSITION_H

#include "bitboard.h"
#include "move.h"
#include "types.h"
#include "zobrist.h"
#include <string>
#include <string_view>

namespace Core {

struct UndoInfo {
  uint64_t savedHash;
  int castlingRights;
  int halfmoveClock;
  Square epSquare;
  PieceType capturedPiece;
#if defined(ENGINE_VARIANTS)
  // Drop variants: the captured piece had been promoted, so it entered the
  // capturer's hand as a pawn and must be restored as its promoted type.
  bool capturedWasPromoted;
#endif
};

class Position {
public:
  static constexpr int MAX_GAME_PLY = 1024;

  Position();

  bool setFromFEN(std::string_view fen);
  std::string toFEN() const;

  void make_move(Move m, UndoInfo &ui);
  void unmake_move(Move m, const UndoInfo &ui);

  void make_null_move(UndoInfo &ui);
  void unmake_null_move(const UndoInfo &ui);

  Bitboard pieces(PieceType pt, Color c) const {
    return byType[pt] & byColor[c];
  }

  Bitboard pieces(Color c) const { return byColor[c]; }

  Bitboard pieces(PieceType pt) const { return byType[pt]; }

  Bitboard occupancy() const { return byColor[WHITE] | byColor[BLACK]; }

  PieceType piece_on(Square s) const { return board[s]; }
  Color color_on(Square s) const;

#if defined(ENGINE_VARIANTS)
  // Standard by default: with no drop variant selected no drop code runs,
  // hands stay empty and hash-neutral, and standard play is identical to
  // before drops existed. Set before setFromFEN so the hand field parses.
  // The whole block compiles out unless ENGINE_VARIANTS is defined, so the
  // standard binary carries neither the state nor the branches.
  void set_variant(Variant v) {
    variant = v;
    drops = (v != VARIANT_STANDARD);
    standardDraws = (v != VARIANT_BUGHOUSE);
    handFromCaptures = (v == VARIANT_CRAZYHOUSE);
  }
  Variant variant_type() const { return variant; }
  // True for crazyhouse and bughouse alike -- the board mechanics are shared.
  bool has_drops() const { return drops; }

  // Draw-rule policy, selectable independently of the variant. Real bughouse
  // has neither a repetition nor a fifty-move draw, because the partner board
  // keeps the game state moving; a host that adjudicates draws per board may
  // want the standard rules anyway. set_variant picks the variant-appropriate
  // default, so call this after it to override.
  void set_standard_draws(bool v) { standardDraws = v; }
  bool standard_draws() const { return standardDraws; }

  // Crazyhouse: a captured piece enters the capturer's own reserve. Bughouse:
  // it enters the *partner's* reserve on the other board, which this engine
  // cannot see -- so captures add nothing here and the reserve changes only
  // when the host injects one via the FEN. Callers learn what a capture
  // produced from UndoInfo (see captured_drop_type).
  bool captures_fill_hand() const { return handFromCaptures; }

  // The piece type a capture sends to a reserve, after the promoted-piece
  // revert (a captured promoted queen becomes a pawn). NO_PIECE_TYPE when the
  // move captured nothing.
  static PieceType captured_drop_type(Move m, const UndoInfo &ui) {
    if (!m.is_capture() || ui.capturedPiece == NO_PIECE_TYPE)
      return NO_PIECE_TYPE;
    return ui.capturedWasPromoted ? PAWN : ui.capturedPiece;
  }

  // Times the current position occurred earlier inside the fifty-move window.
  // Threefold is a count of 2 or more. Distinct from is_repetition(), which is
  // the search's twofold convention.
  int repetition_count() const;

  int in_hand(Color c, PieceType pt) const { return hand[c][pt]; }
  bool has_any_in_hand(Color c) const { return handCount[c] != 0; }
  // Squares holding a piece that reached its type by promotion; captured,
  // such a piece returns to the capturer's hand as a pawn.
  Bitboard promoted_pieces() const { return promoted; }
#else
  static constexpr bool has_drops() { return false; }
  static constexpr int in_hand(Color, PieceType) { return 0; }
  static constexpr bool has_any_in_hand(Color) { return false; }
  // Standard chess always uses the standard draw rules, so the search's
  // fifty-move test folds to a constant in the tournament build.
  static constexpr bool standard_draws() { return true; }
#endif

  Color side_to_move() const { return sideToMove; }
  Square ep_square() const { return epSquare; }
  int castling_rights() const { return castlingRights; }
  int halfmove_clock() const { return halfmoveClock; }
  int fullmove_number() const { return fullmoveNumber; }
  uint64_t hash() const { return zobristHash; }

  // The Zobrist key the position would have after playing m, computed
  // without mutating the board. Lets the search prefetch the child's
  // transposition-table bucket before make_move, so the memory fetch
  // overlaps the board update. Must equal hash() after make_move(m).
  uint64_t key_after(Move m) const;

  // Incremental white-minus-black scores. Material and piece-square
  // bonuses are tracked separately so NNUE blending can use the
  // positional part without double-counting material.
  int material_wb() const { return materialWb; }
  int psqt_wb() const { return psqtWb; }

  bool has_non_pawn_material(Color c) const {
    return (byColor[c] & (byType[KNIGHT] | byType[BISHOP] | byType[ROOK] |
                          byType[QUEEN])) != 0;
  }

  // True if this position occurred at least once before within the
  // fifty-move window (twofold: standard draw convention inside search).
  bool is_repetition() const;

  Bitboard attackers_to(Square s, Bitboard occupied) const;
  Bitboard attackers_to(Square s) const { return attackers_to(s, occupancy()); }

  bool is_square_attacked(Square s, Color attackingSide) const;
  bool in_check() const;

  // False when the side that just moved left its own king in check,
  // i.e. the position is illegal to search (side to move could capture
  // the enemy king). Used to reject malformed FENs at the UCI boundary.
  bool opponent_in_check() const;

  bool is_ok() const;

private:
  Bitboard byColor[COLOR_NB];
  Bitboard byType[PIECE_TYPE_NB];
  PieceType board[SQUARE_NB];

  Color sideToMove;
  int castlingRights;
  Square epSquare;
  int halfmoveClock;
  int fullmoveNumber;
  uint64_t zobristHash;
  int materialWb;
  int psqtWb;

  uint64_t history[MAX_GAME_PLY];
  int gamePly;

#if defined(ENGINE_VARIANTS)
  // Drop-variant state. `hand` counts captured pieces available to drop,
  // `handCount` is the per-colour total so movegen can skip drop generation
  // with one compare, and `promoted` tracks pieces that were promoted.
  // `drops` caches `variant != VARIANT_STANDARD` so the hot paths test one
  // bool rather than comparing the enum.
  Variant variant;
  bool drops;
  bool standardDraws;
  bool handFromCaptures;
  uint8_t hand[COLOR_NB][PIECE_TYPE_NB];
  uint8_t handCount[COLOR_NB];
  Bitboard promoted;
#endif

  void put_piece(PieceType pt, Color c, Square s);
  void remove_piece(Color c, Square s);
  void move_piece(Color c, Square from, Square to);

#if defined(ENGINE_VARIANTS)
  void add_to_hand(Color c, PieceType pt);
  void remove_from_hand(Color c, PieceType pt);
#endif
};

} // namespace Core

#endif
