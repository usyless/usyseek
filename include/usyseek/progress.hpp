#pragma once

#include <cstdint>

namespace usyseek {

struct DisplayedProgress {
    uint64_t embedded = 0;
    uint64_t total = 0;
};

inline DisplayedProgress displayed_progress(bool indexer_running, uint64_t live_embedded, uint64_t live_found,
                                            uint64_t at_rest_embedded, uint64_t at_rest_total) noexcept {
    const bool live_counts_ready = indexer_running && live_found > 0;
    if (live_counts_ready) return {live_embedded, live_found};
    return {at_rest_embedded, at_rest_total};
}

} // namespace usyseek
