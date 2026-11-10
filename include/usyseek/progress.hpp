#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace usyseek {

struct DisplayedProgress {
    uint64_t embedded = 0;
    uint64_t total = 0;
};

inline DisplayedProgress displayed_progress(bool indexer_running, uint64_t live_embedded, uint64_t live_found,
                                            std::optional<uint64_t> state_embedded, uint64_t state_found,
                                            std::optional<uint64_t> db_count) noexcept {
    const bool live_counts_ready = indexer_running && live_found > 0;
    if (live_counts_ready) return {live_embedded, live_found};
    if (state_embedded) return {*state_embedded, state_found};
    const uint64_t embedded = db_count.value_or(state_found);
    return {embedded, std::max(embedded, state_found)};
}

} // namespace usyseek
