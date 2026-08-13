#include "position.h"
#include "attacks.h"
#include "invariants.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace Core {

constexpr int CASTLE_WK = 1;
constexpr int CASTLE_WQ = 2;
constexpr int CASTLE_BK = 4;
constexpr int CASTLE_BQ = 8;

int CastlingSpoilers[64];
bool SpoilersInitialized = false;

void init_spoilers() {
  if (SpoilersInitialized)
    return;

  for (int i = 0; i < 64; ++i)
    CastlingSpoilers[i] = 15;

  CastlingSpoilers[SQ_A1] &= ~CASTLE_WQ;
  CastlingSpoilers[SQ_E1] &= ~(CASTLE_WK | CASTLE_WQ);
  CastlingSpoilers[SQ_H1] &= ~CASTLE_WK;

  CastlingSpoilers[SQ_A8] &= ~CASTLE_BQ;
  CastlingSpoilers[SQ_E8] &= ~(CASTLE_BK | CASTLE_BQ);
  CastlingSpoilers[SQ_H8] &= ~CASTLE_BK;

  SpoilersInitialized = true;
}

// Piece-square bonus tables, indexed [color][type][square]. The running
// white-minus-black totals let evaluate() reduce to a sign flip.
int PsqTable[COLOR_NB][PIECE_TYPE_NB][SQUARE_NB];
bool PsqInitialized = false;

namespace {
int psq_center_bonus(Square sq) {
  const int file = file_of(sq);
  const int rank = rank_of(sq);
  const int dist = std::abs(file - 3) + std::abs(rank - 3);
  return 14 - 3 * dist;
}

int psq_bonus(PieceType pt, Square sq, Color c) {
  const int file = file_of(sq);
  const int rank = rank_of(sq);
  const int relRank = c == WHITE ? rank : (7 - rank);
  const int center = psq_center_bonus(sq);

  switch (pt) {
  case PAWN:
    return relRank * 6 - std::abs(file - 3) * 2;
  case KNIGHT:
    return center;
  case BISHOP:
    return center / 2;
  case ROOK:
    return relRank * 3;
  case QUEEN:
    return center / 3;
  case KING:
    return -(center / 2);
  default:
    return 0;
  }
}
} // namespace

void init_psq_tables() {
  if (PsqInitialized)
    return;
  for (int c = WHITE; c <= BLACK; ++c) {
    for (int pt = NO_PIECE_TYPE; pt < PIECE_TYPE_NB; ++pt) {
      for (int s = 0; s < SQUARE_NB; ++s) {
        const PieceType type = static_cast<PieceType>(pt);
        const Square sq = static_cast<Square>(s);
        PsqTable[c][pt][s] = (pt == NO_PIECE_TYPE)
                                 ? 0
                                 : psq_bonus(type, sq, static_cast<Color>(c));
      }
    }
  }
  PsqInitialized = true;
}

Position::Position() {

  init_spoilers();
  init_psq_tables();

  std::memset(byColor, 0, sizeof(byColor));
  std::memset(byType, 0, sizeof(byType));
  std::memset(history, 0, sizeof(history));
  std::fill(std::begin(board), std::end(board), NO_PIECE_TYPE);

  sideToMove = WHITE;
  castlingRights = 0;
  epSquare = SQ_NONE;
  halfmoveClock = 0;
  fullmoveNumber = 1;
  zobristHash = 0;
  materialWb = 0;
  psqtWb = 0;
  gamePly = 0;

#if defined(ENGINE_VARIANTS)
  variant = VARIANT_STANDARD;
  drops = false;
  standardDraws = true;
  handFromCaptures = false;
  std::memset(hand, 0, sizeof(hand));
  std::memset(handCount, 0, sizeof(handCount));
  promoted = 0;
#endif
}

#if defined(ENGINE_VARIANTS)
void Position::add_to_hand(Color c, PieceType pt) {
  uint8_t &n = hand[c][pt];
  if (n < MAX_IN_HAND)
    zobristHash ^= Zobrist::hand[c][pt][n] ^ Zobrist::hand[c][pt][n + 1];
  ++n;
  ++handCount[c];
}

void Position::remove_from_hand(Color c, PieceType pt) {
  uint8_t &n = hand[c][pt];
  if (n <= MAX_IN_HAND)
    zobristHash ^= Zobrist::hand[c][pt][n] ^ Zobrist::hand[c][pt][n - 1];
  --n;
  --handCount[c];
}
#endif

void Position::put_piece(PieceType pt, Color c, Square s) {
  if (pt == NO_PIECE_TYPE)
    return;
  Bitboard bb = square_bb(s);
  byColor[c] |= bb;
  byType[pt] |= bb;
  board[s] = pt;
  const int sign = (c == WHITE) ? 1 : -1;
  materialWb += sign * PieceValue[pt];
  psqtWb += sign * PsqTable[c][pt][s];
  zobristHash ^= Zobrist::psq[c][pt][s];
}

void Position::remove_piece(Color c, Square s) {
  const PieceType pt = board[s];
  if (pt == NO_PIECE_TYPE)
    return;

  Bitboard bb = square_bb(s);
  byColor[c] ^= bb;
  byType[pt] ^= bb;
  board[s] = NO_PIECE_TYPE;
  const int sign = (c == WHITE) ? 1 : -1;
  materialWb -= sign * PieceValue[pt];
  psqtWb -= sign * PsqTable[c][pt][s];
  zobristHash ^= Zobrist::psq[c][pt][s];

#if defined(ENGINE_VARIANTS)
  if (drops && (promoted & bb)) {
    promoted ^= bb;
    zobristHash ^= Zobrist::promotedSq[s];
  }
#endif
}

void Position::move_piece(Color c, Square from, Square to) {
  const PieceType pt = board[from];
  if (pt == NO_PIECE_TYPE)
    return;

  Bitboard from_bb = square_bb(from);
  Bitboard to_bb = square_bb(to);
  Bitboard mask = from_bb | to_bb;

  byType[pt] ^= mask;
  byColor[c] ^= mask;
  board[from] = NO_PIECE_TYPE;
  board[to] = pt;

  const int delta = PsqTable[c][pt][to] - PsqTable[c][pt][from];
  psqtWb += (c == WHITE) ? delta : -delta;

  zobristHash ^= Zobrist::psq[c][pt][from];
  zobristHash ^= Zobrist::psq[c][pt][to];

#if defined(ENGINE_VARIANTS)
  // Promotion status travels with the piece. Handling it here rather than in
  // make_move covers the castling rook and the unmake path for free.
  if (drops && (promoted & from_bb)) {
    promoted ^= mask;
    zobristHash ^= Zobrist::promotedSq[from] ^ Zobrist::promotedSq[to];
  }
#endif
}

Color Position::color_on(Square s) const {
  Bitboard bb = square_bb(s);
  if (byColor[WHITE] & bb)
    return WHITE;
  if (byColor[BLACK] & bb)
    return BLACK;
  return COLOR_NB;
}

bool Position::setFromFEN(std::string_view fen) {
  std::memset(byColor, 0, sizeof(byColor));
  std::memset(byType, 0, sizeof(byType));
  std::fill(std::begin(board), std::end(board), NO_PIECE_TYPE);
  zobristHash = 0;
  materialWb = 0;
  psqtWb = 0;
  gamePly = 0;
#if defined(ENGINE_VARIANTS)
  std::memset(hand, 0, sizeof(hand));
  std::memset(handCount, 0, sizeof(handCount));
  promoted = 0;
#endif

  std::stringstream ss(std::string{fen});
  std::string board_str, side, castle, ep, half, full;

  ss >> board_str >> side >> castle >> ep >> half >> full;

#if defined(ENGINE_VARIANTS)
  // Crazyhouse and bughouse FEN carry the reserves in brackets appended to
  // the board field, e.g. "8/8/...[QPpp]". Reject it outright in standard
  // mode so a variant FEN can never be half-loaded into a standard search.
  std::string hand_str;
  const size_t bracket = board_str.find('[');
  if (bracket != std::string::npos) {
    if (!drops)
      return false;
    const size_t close = board_str.find(']', bracket);
    if (close == std::string::npos)
      return false;
    hand_str = board_str.substr(bracket + 1, close - bracket - 1);
    board_str.erase(bracket);
  }
  Square lastSquare = SQ_NONE;
#endif

  int rank = 7;
  int file = 0;
  for (char c : board_str) {
    if (c == '/') {
      rank--;
      file = 0;
    } else if (isdigit(c)) {
      file += (c - '0');
#if defined(ENGINE_VARIANTS)
    } else if (c == '~') {
      // Marks the piece just placed as promoted.
      if (!drops || lastSquare == SQ_NONE)
        return false;
      promoted |= square_bb(lastSquare);
      zobristHash ^= Zobrist::promotedSq[lastSquare];
#endif
    } else {
      Color color = isupper(c) ? WHITE : BLACK;
      PieceType pt = NO_PIECE_TYPE;
      switch (tolower(c)) {
      case 'p':
        pt = PAWN;
        break;
      case 'n':
        pt = KNIGHT;
        break;
      case 'b':
        pt = BISHOP;
        break;
      case 'r':
        pt = ROOK;
        break;
      case 'q':
        pt = QUEEN;
        break;
      case 'k':
        pt = KING;
        break;
      default:
        break;
      }
#if defined(ENGINE_VARIANTS)
      lastSquare = make_square((GenFile)file, (GenRank)rank);
      put_piece(pt, color, lastSquare);
#else
      put_piece(pt, color, make_square((GenFile)file, (GenRank)rank));
#endif
      file++;
    }
  }

#if defined(ENGINE_VARIANTS)
  for (char c : hand_str) {
    const Color color = isupper(c) ? WHITE : BLACK;
    PieceType pt = NO_PIECE_TYPE;
    switch (tolower(c)) {
    case 'p':
      pt = PAWN;
      break;
    case 'n':
      pt = KNIGHT;
      break;
    case 'b':
      pt = BISHOP;
      break;
    case 'r':
      pt = ROOK;
      break;
    case 'q':
      pt = QUEEN;
      break;
    default:
      return false; // kings are never in hand
    }
    add_to_hand(color, pt);
  }
#endif

  sideToMove = (side == "w") ? WHITE : BLACK;
  if (sideToMove == BLACK)
    zobristHash ^= Zobrist::side;

  castlingRights = 0;
  if (castle != "-") {
    for (char c : castle) {
      if (c == 'K')
        castlingRights |= CASTLE_WK;
      if (c == 'Q')
        castlingRights |= CASTLE_WQ;
      if (c == 'k')
        castlingRights |= CASTLE_BK;
      if (c == 'q')
        castlingRights |= CASTLE_BQ;
    }
  }
  zobristHash ^= Zobrist::castling[castlingRights];

  epSquare = SQ_NONE;
  if (ep != "-") {
    GenFile f = (GenFile)(ep[0] - 'a');
    GenRank r = (GenRank)(ep[1] - '1');
    epSquare = make_square(f, r);
    zobristHash ^= Zobrist::enpassant[f];
  }

  try {
    halfmoveClock = half.empty() ? 0 : std::stoi(half);
    fullmoveNumber = full.empty() ? 1 : std::stoi(full);
  } catch (...) {
    halfmoveClock = 0;
    fullmoveNumber = 1;
  }

  history[gamePly] = zobristHash;

  return is_ok();
}

std::string Position::toFEN() const {
  std::stringstream ss;
  for (int r = RANK_8; r >= RANK_1; --r) {
    int emptyCount = 0;
    for (int f = FILE_A; f <= FILE_H; ++f) {
      Square s = make_square((GenFile)f, (GenRank)r);
      PieceType pt = piece_on(s);
      Color c = color_on(s);

      if (pt == NO_PIECE_TYPE) {
        emptyCount++;
      } else {
        if (emptyCount > 0) {
          ss << emptyCount;
          emptyCount = 0;
        }
        char pieceChar = '?';
        switch (pt) {
        case PAWN:
          pieceChar = 'p';
          break;
        case KNIGHT:
          pieceChar = 'n';
          break;
        case BISHOP:
          pieceChar = 'b';
          break;
        case ROOK:
          pieceChar = 'r';
          break;
        case QUEEN:
          pieceChar = 'q';
          break;
        case KING:
          pieceChar = 'k';
          break;
        default:
          break;
        }
        if (c == WHITE)
          pieceChar = static_cast<char>(toupper(pieceChar));
        ss << pieceChar;
#if defined(ENGINE_VARIANTS)
        if (drops && (promoted & square_bb(s)))
          ss << '~';
#endif
      }
    }
    if (emptyCount > 0)
      ss << emptyCount;
    if (r > RANK_1)
      ss << '/';
  }
#if defined(ENGINE_VARIANTS)
  if (drops) {
    ss << '[';
    static constexpr char kHandChars[PIECE_TYPE_NB] = {0,   'p', 'n', 'b',
                                                       'r', 'q', 0};
    for (int c = WHITE; c <= BLACK; ++c) {
      for (int pt = QUEEN; pt >= PAWN; --pt) {
        const char ch = (c == WHITE)
                            ? static_cast<char>(toupper(kHandChars[pt]))
                            : kHandChars[pt];
        for (int n = 0; n < hand[c][pt]; ++n)
          ss << ch;
      }
    }
    ss << ']';
  }
#endif
  ss << (sideToMove == WHITE ? " w " : " b ");
  if (castlingRights == 0) {
    ss << "-";
  } else {
    if (castlingRights & CASTLE_WK)
      ss << 'K';
    if (castlingRights & CASTLE_WQ)
      ss << 'Q';
    if (castlingRights & CASTLE_BK)
      ss << 'k';
    if (castlingRights & CASTLE_BQ)
      ss << 'q';
  }
  ss << " ";
  if (epSquare == SQ_NONE) {
    ss << "-";
  } else {
    char f = static_cast<char>('a' + file_of(epSquare));
    char r = static_cast<char>('1' + rank_of(epSquare));
    ss << f << r;
  }
  ss << " " << halfmoveClock << " " << fullmoveNumber;
  return ss.str();
}

void Position::make_move(Move m, UndoInfo &ui) {
  ASSERT_CONSISTENCY(*this);

  Square to = m.to_sq();

  ui.capturedPiece = NO_PIECE_TYPE;
#if defined(ENGINE_VARIANTS)
  ui.capturedWasPromoted = false;
#endif
  ui.castlingRights = castlingRights;
  ui.epSquare = epSquare;
  ui.halfmoveClock = halfmoveClock;
  ui.savedHash = zobristHash;

  bool resetClock = false;
  Square newEpSquare = SQ_NONE;

#if defined(ENGINE_VARIANTS)
  if (m.is_drop()) {
    // A drop is irreversible, so it resets the clock; it never creates an
    // en-passant square, and it cannot spoil castling rights, because every
    // square that would spoil them is occupied whenever those rights exist.
    const PieceType dropped = m.dropped_piece();
    remove_from_hand(sideToMove, dropped);
    put_piece(dropped, sideToMove, to);
    resetClock = true;
  } else
#endif
  {
    Square from = m.from_sq();
    PieceType movingPiece = board[from];

    if (movingPiece == PAWN)
      resetClock = true;

    if (m.is_capture()) {
      resetClock = true;
      Square capSq = to;
      if (m.is_en_passant()) {
        capSq = make_square((GenFile)file_of(to), (GenRank)rank_of(from));
      }
      ui.capturedPiece = board[capSq];
#if defined(ENGINE_VARIANTS)
      if (drops) {
        // A promoted piece reverts to a pawn in the reserve. Read the flag
        // before remove_piece clears it -- it is reported to the host even in
        // bughouse, where the piece goes to the partner board's reserve
        // rather than this one.
        ui.capturedWasPromoted = (promoted & square_bb(capSq)) != 0;
        if (handFromCaptures)
          add_to_hand(sideToMove,
                      ui.capturedWasPromoted ? PAWN : ui.capturedPiece);
      }
#endif
      remove_piece(~sideToMove, capSq);
      castlingRights &= CastlingSpoilers[to];
    }

    move_piece(sideToMove, from, to);

    if (m.is_promotion()) {
      remove_piece(sideToMove, to);
      put_piece(m.promotion_type(), sideToMove, to);
#if defined(ENGINE_VARIANTS)
      if (drops) {
        promoted |= square_bb(to);
        zobristHash ^= Zobrist::promotedSq[to];
      }
#endif
    }

    if (m.is_castling()) {
      Square rFrom, rTo;
      if (to > from) {
        rFrom = make_square(FILE_H, (GenRank)rank_of(from));
        rTo = make_square(FILE_F, (GenRank)rank_of(from));
      } else {
        rFrom = make_square(FILE_A, (GenRank)rank_of(from));
        rTo = make_square(FILE_D, (GenRank)rank_of(from));
      }
      move_piece(sideToMove, rFrom, rTo);
    }

    castlingRights &= CastlingSpoilers[from];

    if (m.is_double_push()) {
      newEpSquare =
          (sideToMove == WHITE) ? (Square)(from + 8) : (Square)(from - 8);
    }
  }

  if (epSquare != SQ_NONE)
    zobristHash ^= Zobrist::enpassant[file_of(epSquare)];
  epSquare = newEpSquare;
  if (epSquare != SQ_NONE)
    zobristHash ^= Zobrist::enpassant[file_of(epSquare)];

  zobristHash ^= Zobrist::castling[ui.castlingRights];
  zobristHash ^= Zobrist::castling[castlingRights];

  if (resetClock)
    halfmoveClock = 0;
  else
    halfmoveClock++;

  if (sideToMove == BLACK)
    fullmoveNumber++;
  sideToMove = ~sideToMove;
  zobristHash ^= Zobrist::side;

  gamePly++;
  if (gamePly < MAX_GAME_PLY)
    history[gamePly] = zobristHash;
  ASSERT_CONSISTENCY(*this);
}

uint64_t Position::key_after(Move m) const {
  const Square to = m.to_sq();
  const Color us = sideToMove;
  const Color them = ~us;

  uint64_t k = zobristHash;

#if defined(ENGINE_VARIANTS)
  if (m.is_drop()) {
    const PieceType dropped = m.dropped_piece();
    const int n = hand[us][dropped];
    k ^= Zobrist::psq[us][dropped][to];
    if (n <= MAX_IN_HAND)
      k ^= Zobrist::hand[us][dropped][n] ^ Zobrist::hand[us][dropped][n - 1];
    if (epSquare != SQ_NONE)
      k ^= Zobrist::enpassant[file_of(epSquare)];
    k ^= Zobrist::side;
    return k;
  }
#endif

  const Square from = m.from_sq();
  const PieceType movingPiece = board[from];
  int newCastling = castlingRights;

  if (m.is_capture()) {
    Square capSq = to;
    PieceType captured;
    if (m.is_en_passant()) {
      capSq = make_square((GenFile)file_of(to), (GenRank)rank_of(from));
      captured = PAWN;
    } else {
      captured = board[to];
    }
    k ^= Zobrist::psq[them][captured][capSq];
    newCastling &= CastlingSpoilers[to];

#if defined(ENGINE_VARIANTS)
    if (drops) {
      const bool wasPromoted = (promoted & square_bb(capSq)) != 0;
      if (handFromCaptures) {
        const PieceType toHand = wasPromoted ? PAWN : captured;
        const int n = hand[us][toHand];
        if (n < MAX_IN_HAND)
          k ^= Zobrist::hand[us][toHand][n] ^ Zobrist::hand[us][toHand][n + 1];
      }
      if (wasPromoted)
        k ^= Zobrist::promotedSq[capSq];
    }
#endif
  }

  // Moving piece leaves `from` and lands on `to`; a promotion changes
  // which piece lands (the intermediate pawn-on-`to` term cancels out).
  k ^= Zobrist::psq[us][movingPiece][from];
  k ^=
      Zobrist::psq[us][m.is_promotion() ? m.promotion_type() : movingPiece][to];

#if defined(ENGINE_VARIANTS)
  if (drops) {
    // Promotion marks `to`; an already-promoted piece carries its mark along.
    if (m.is_promotion())
      k ^= Zobrist::promotedSq[to];
    else if (promoted & square_bb(from))
      k ^= Zobrist::promotedSq[from] ^ Zobrist::promotedSq[to];
  }
#endif

  if (m.is_castling()) {
    Square rFrom, rTo;
    if (to > from) {
      rFrom = make_square(FILE_H, (GenRank)rank_of(from));
      rTo = make_square(FILE_F, (GenRank)rank_of(from));
    } else {
      rFrom = make_square(FILE_A, (GenRank)rank_of(from));
      rTo = make_square(FILE_D, (GenRank)rank_of(from));
    }
    k ^= Zobrist::psq[us][ROOK][rFrom];
    k ^= Zobrist::psq[us][ROOK][rTo];
  }

  newCastling &= CastlingSpoilers[from];
  k ^= Zobrist::castling[castlingRights];
  k ^= Zobrist::castling[newCastling];

  if (epSquare != SQ_NONE)
    k ^= Zobrist::enpassant[file_of(epSquare)];
  if (m.is_double_push()) {
    const Square epCand =
        (us == WHITE) ? (Square)(from + 8) : (Square)(from - 8);
    k ^= Zobrist::enpassant[file_of(epCand)];
  }

  k ^= Zobrist::side;
  return k;
}

void Position::unmake_move(Move m, const UndoInfo &ui) {
  gamePly--;
  sideToMove = ~sideToMove;
  if (sideToMove == BLACK)
    fullmoveNumber--;

  Square to = m.to_sq();

  // Restore pieces through the shared helpers so the mailbox and the
  // incremental psq score stay consistent; the hash they touch is
  // overwritten by the saved value below.
#if defined(ENGINE_VARIANTS)
  if (m.is_drop()) {
    remove_piece(sideToMove, to);
    add_to_hand(sideToMove, m.dropped_piece());
    zobristHash = ui.savedHash;
    epSquare = ui.epSquare;
    castlingRights = ui.castlingRights;
    halfmoveClock = ui.halfmoveClock;
    ASSERT_CONSISTENCY(*this);
    return;
  }
#endif

  Square from = m.from_sq();

  if (m.is_promotion()) {
    remove_piece(sideToMove, to);
    put_piece(PAWN, sideToMove, from);
  } else {
    move_piece(sideToMove, to, from);
  }

  if (m.is_castling()) {
    Square rFrom, rTo;
    if (to > from) {
      rFrom = make_square(FILE_H, (GenRank)rank_of(from));
      rTo = make_square(FILE_F, (GenRank)rank_of(from));
    } else {
      rFrom = make_square(FILE_A, (GenRank)rank_of(from));
      rTo = make_square(FILE_D, (GenRank)rank_of(from));
    }
    move_piece(sideToMove, rTo, rFrom);
  }

  if (m.is_capture() && ui.capturedPiece != NO_PIECE_TYPE) {
    Square capSq = to;
    if (m.is_en_passant()) {
      capSq = make_square((GenFile)file_of(to), (GenRank)rank_of(from));
    }
    put_piece(ui.capturedPiece, ~sideToMove, capSq);
#if defined(ENGINE_VARIANTS)
    if (drops) {
      if (handFromCaptures)
        remove_from_hand(sideToMove,
                         ui.capturedWasPromoted ? PAWN : ui.capturedPiece);
      if (ui.capturedWasPromoted)
        promoted |= square_bb(capSq);
    }
#endif
  }

  zobristHash = ui.savedHash;
  epSquare = ui.epSquare;
  castlingRights = ui.castlingRights;
  halfmoveClock = ui.halfmoveClock;
  ASSERT_CONSISTENCY(*this);
}

void Position::make_null_move(UndoInfo &ui) {
  ASSERT_CONSISTENCY(*this);
  ui.capturedPiece = NO_PIECE_TYPE;
  ui.castlingRights = castlingRights;
  ui.epSquare = epSquare;
  ui.halfmoveClock = halfmoveClock;
  ui.savedHash = zobristHash;

  if (epSquare != SQ_NONE) {
    zobristHash ^= Zobrist::enpassant[file_of(epSquare)];
    epSquare = SQ_NONE;
  }

  sideToMove = ~sideToMove;
  zobristHash ^= Zobrist::side;
  gamePly++;
  if (gamePly < MAX_GAME_PLY)
    history[gamePly] = zobristHash;
}

void Position::unmake_null_move(const UndoInfo &ui) {
  gamePly--;
  sideToMove = ~sideToMove;
  zobristHash = ui.savedHash;
  epSquare = ui.epSquare;
  ASSERT_CONSISTENCY(*this);
}

#if defined(ENGINE_VARIANTS)
int Position::repetition_count() const {
  if (!standardDraws)
    return 0;
  if (halfmoveClock < 4)
    return 0;
  int n = 0;
  const int start = std::max(0, gamePly - halfmoveClock);
  for (int i = gamePly - 2; i >= start; i -= 2) {
    if (history[i] == zobristHash)
      ++n;
  }
  return n;
}
#endif

bool Position::is_repetition() const {
  // Bughouse has no repetition draw: a repeated position on this board is
  // not a repeated game state, because the partner board has moved on.
  // Crazyhouse keeps the normal rule -- hands are hashed, so a repeated key
  // really is a repeated position.
#if defined(ENGINE_VARIANTS)
  if (!standardDraws)
    return false;
#endif
  // A repetition needs at least four reversible plies since the last
  // capture or pawn move; skip the history walk entirely below that.
  if (halfmoveClock < 4)
    return false;
  const int start = std::max(0, gamePly - halfmoveClock);
  for (int i = gamePly - 2; i >= start; i -= 2) {
    if (history[i] == zobristHash)
      return true;
  }
  return false;
}

Bitboard Position::attackers_to(Square s, Bitboard occupied) const {
  return (Attacks::pawn_attacks(s, BLACK) & byColor[WHITE] & byType[PAWN]) |
         (Attacks::pawn_attacks(s, WHITE) & byColor[BLACK] & byType[PAWN]) |
         (Attacks::knight_attacks(s) & byType[KNIGHT]) |
         (Attacks::king_attacks(s) & byType[KING]) |
         (Attacks::bishop_attacks(s, occupied) &
          (byType[BISHOP] | byType[QUEEN])) |
         (Attacks::rook_attacks(s, occupied) & (byType[ROOK] | byType[QUEEN]));
}

bool Position::is_square_attacked(Square s, Color attackingSide) const {
  if (Attacks::pawn_attacks(s, ~attackingSide) & pieces(PAWN, attackingSide))
    return true;
  if (Attacks::knight_attacks(s) & pieces(KNIGHT, attackingSide))
    return true;
  if (Attacks::king_attacks(s) & pieces(KING, attackingSide))
    return true;

  Bitboard occ = occupancy();
  if (Attacks::bishop_attacks(s, occ) &
      (pieces(BISHOP, attackingSide) | pieces(QUEEN, attackingSide)))
    return true;
  if (Attacks::rook_attacks(s, occ) &
      (pieces(ROOK, attackingSide) | pieces(QUEEN, attackingSide)))
    return true;

  return false;
}

bool Position::in_check() const {
  Bitboard k = pieces(KING, sideToMove);
  if (k == 0)
    return false;
  Square kSq = lsb(k);
  return is_square_attacked(kSq, ~sideToMove);
}

bool Position::opponent_in_check() const {
  Bitboard k = pieces(KING, ~sideToMove);
  if (k == 0)
    return true; // no enemy king == not a legal position
  return is_square_attacked(lsb(k), sideToMove);
}

bool Position::is_ok() const {
  if (byColor[WHITE] & byColor[BLACK])
    return false;
  if (popcount(byType[KING] & byColor[WHITE]) != 1)
    return false;
  if (popcount(byType[KING] & byColor[BLACK]) != 1)
    return false;
  if (epSquare != SQ_NONE) {
    if (sideToMove == WHITE) {
      if (rank_of(epSquare) != RANK_6)
        return false;
    } else {
      if (rank_of(epSquare) != RANK_3)
        return false;
    }
  }
  for (int s = 0; s < SQUARE_NB; ++s) {
    const Bitboard bb = square_bb(static_cast<Square>(s));
    const PieceType pt = board[s];
    if (pt == NO_PIECE_TYPE) {
      if (occupancy() & bb)
        return false;
    } else if (!(byType[pt] & bb)) {
      return false;
    }
  }
  return true;
}

} // namespace Core
