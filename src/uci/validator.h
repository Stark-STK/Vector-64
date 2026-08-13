#ifndef UCI_VALIDATOR_H
#define UCI_VALIDATOR_H

#if defined(ENGINE_VARIANTS)

#include "../cores/material.h"
#include "../cores/movegen.h"
#include "../cores/position.h"
#include "uci_util.h"

#include <string>

// Structured request/response surface for a host that treats the engine as the
// single source of chess truth: legality, move application, legal-move
// enumeration, terminal detection and mating-material checks, with no search
// and no NNUE.
//
// Responses are one line of compact JSON, hand-built so that `cores` gains no
// third-party dependency. Every response carries "ok"; a failure also carries
// "error" drawn from a fixed taxonomy the host can branch on:
//
//   illegal_move       well-formed request, the move is not legal here
//   malformed_request  unparseable move, side, or arguments
//   internal_error     engine fault; the host should evict the worker
//
// Commands (each consumes the caller-supplied position, so a worker holds no
// game identity and any worker can answer any request):
//
//   getfen                 -> {"ok":true,"fen":...,"variant":...,"drawRules":...}
//   legalmoves             -> {"ok":true,"count":N,"moves":[...]}
//   apply <uci>            -> {"ok":true,"legal":true,"fen":...,"sideToMove":...,
//                              "inCheck":...,"terminal":...,"capturedDropType":...}
//   status                 -> {"ok":true,"sideToMove":...,"inCheck":...,
//                              "terminal":...,"legalCount":N,"repetitions":k}
//   canmate <w|b>          -> {"ok":true,"side":...,"canMate":...}

namespace UCI::Validator {

// Error codes, as emitted in the "error" field.
constexpr const char *ERR_ILLEGAL = "illegal_move";
constexpr const char *ERR_MALFORMED = "malformed_request";
constexpr const char *ERR_INTERNAL = "internal_error";

inline std::string json_error(const char *code, const std::string &reason) {
  return std::string("{\"ok\":false,\"error\":\"") + code + "\",\"reason\":\"" +
         reason + "\"}";
}

inline const char *variant_name(Core::Variant v) {
  switch (v) {
  case Core::VARIANT_CRAZYHOUSE:
    return "crazyhouse";
  case Core::VARIANT_BUGHOUSE:
    return "bughouse";
  default:
    return "chess";
  }
}

inline char drop_type_char(Core::PieceType pt) {
  switch (pt) {
  case Core::PAWN:
    return 'p';
  case Core::KNIGHT:
    return 'n';
  case Core::BISHOP:
    return 'b';
  case Core::ROOK:
    return 'r';
  case Core::QUEEN:
    return 'q';
  default:
    return '-';
  }
}

// Terminal state of `pos`, in the host's vocabulary. Checkmate and stalemate
// are decided first because a position with no legal moves is over regardless
// of any draw counter.
inline const char *terminal_state(Core::Position &pos) {
  Core::MoveList legal;
  Core::generate_legal_moves(pos, legal);
  if (legal.size() == 0)
    return pos.in_check() ? "checkmate" : "stalemate";
  if (Core::insufficient_material_both(pos))
    return "draw_insufficient_material";
  if (pos.standard_draws() && pos.halfmove_clock() >= 100)
    return "draw_fifty_move";
  if (pos.repetition_count() >= 2)
    return "draw_threefold";
  return "none";
}

inline std::string json_getfen(const Core::Position &pos) {
  return std::string("{\"ok\":true,\"fen\":\"") + pos.toFEN() +
         "\",\"variant\":\"" + variant_name(pos.variant_type()) +
         "\",\"drawRules\":\"" + (pos.standard_draws() ? "standard" : "variant") +
         "\"}";
}

inline std::string json_legalmoves(Core::Position &pos) {
  Core::MoveList legal;
  Core::generate_legal_moves(pos, legal);
  std::string out = "{\"ok\":true,\"count\":";
  out += std::to_string(legal.size());
  out += ",\"moves\":[";
  for (int i = 0; i < legal.size(); ++i) {
    if (i)
      out += ',';
    out += '"';
    out += move_to_uci(legal[i]);
    out += '"';
  }
  out += "]}";
  return out;
}

inline std::string json_status(Core::Position &pos) {
  Core::MoveList legal;
  Core::generate_legal_moves(pos, legal);
  std::string out = "{\"ok\":true,\"sideToMove\":\"";
  out += (pos.side_to_move() == Core::WHITE ? "w" : "b");
  out += "\",\"inCheck\":";
  out += (pos.in_check() ? "true" : "false");
  out += ",\"terminal\":\"";
  out += terminal_state(pos);
  out += "\",\"legalCount\":";
  out += std::to_string(legal.size());
  out += ",\"repetitions\":";
  out += std::to_string(pos.repetition_count());
  out += '}';
  return out;
}

inline std::string json_canmate(const Core::Position &pos, Core::Color c) {
  return std::string("{\"ok\":true,\"side\":\"") +
         (c == Core::WHITE ? "w" : "b") + "\",\"canMate\":" +
         (Core::can_side_mate(pos, c) ? "true" : "false") + "}";
}

} // namespace UCI::Validator

#endif // ENGINE_VARIANTS
#endif
