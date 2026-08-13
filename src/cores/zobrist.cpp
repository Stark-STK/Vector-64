#include "zobrist.h"

namespace Core {

namespace Zobrist {
Key psq[COLOR_NB][PIECE_TYPE_NB][64];
Key enpassant[FILE_NB];
Key castling[16];
Key side;
#if defined(ENGINE_VARIANTS)
Key hand[COLOR_NB][PIECE_TYPE_NB][MAX_IN_HAND + 1];
Key promotedSq[64];
#endif

struct PRNG {
  uint64_t s;
  PRNG(uint64_t seed) : s(seed) {}

  uint64_t rand64() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 2685821657736338717LL;
  }
};
void init() {
  PRNG rng(1070372);

  for (int c = 0; c < COLOR_NB; ++c) {
    for (int pt = 0; pt < PIECE_TYPE_NB; ++pt) {
      for (int s = 0; s < 64; ++s) {
        psq[c][pt][s] = rng.rand64();
      }
    }
  }

  for (int f = 0; f < FILE_NB; ++f) {
    enpassant[f] = rng.rand64();
  }

  for (int i = 0; i < 16; ++i) {
    castling[i] = rng.rand64();
  }

  side = rng.rand64();

#if defined(ENGINE_VARIANTS)
  // Variant keys are drawn last on purpose: every draw above keeps the
  // stream position it had before drops existed, so standard-chess hashes
  // -- and the bench signature that depends on them -- are unchanged even
  // in a variant-enabled build.
  // A zero key for an empty hand makes the standard game's hands hash-neutral.
  for (int c = 0; c < COLOR_NB; ++c) {
    for (int pt = 0; pt < PIECE_TYPE_NB; ++pt) {
      hand[c][pt][0] = 0;
      for (int n = 1; n <= MAX_IN_HAND; ++n) {
        hand[c][pt][n] = rng.rand64();
      }
    }
  }

  for (int s = 0; s < 64; ++s) {
    promotedSq[s] = rng.rand64();
  }
#endif
}

} // namespace Zobrist

} // namespace Core
