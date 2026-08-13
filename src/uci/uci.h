#ifndef UCI_H
#define UCI_H

#include <cstddef>

namespace UCI {

// `embeddedNet` (if non-null) is the default net baked into the NNUE binary;
// main() passes it from the embedded image. The classical binary passes null
// and defaults to the material+psqt eval.
// `validatorOnly` starts a lean worker: a minimal transposition table, one
// thread, and `go`/`bench` refused, so a host can keep a resident pool for
// legality and terminal queries that never queues behind a search.
int run(const unsigned char *embeddedNet = nullptr,
        std::size_t embeddedNetSize = 0, bool validatorOnly = false);

} // namespace UCI

#endif
