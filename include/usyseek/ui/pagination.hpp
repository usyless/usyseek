#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace usyseek::ui {

/// Tracks seen point IDs and pagination offsets across result appends so duplicate IDs across
/// pages cannot couple distinct grid cards in `selected_` or stall offset progression.
class PaginationTracker {
public:
    void reset() noexcept {
        seen_ids_.clear();
        next_offset_ = 0;
    }

    [[nodiscard]] uint64_t next_offset() const noexcept { return next_offset_; }
    [[nodiscard]] size_t unique_count() const noexcept { return seen_ids_.size(); }

    /// Applies one result page in order, forwarding unseen items to `on_accept(elem)`.
    /// Returns false when `ok` is false or an appended page fails to advance `next_offset_`.
    template <typename Range, typename IdFn, typename AcceptFn>
    bool apply_page(Range& src, bool append, bool ok, uint64_t result_next_offset,
                    IdFn&& id_of, AcceptFn&& on_accept) {
        if (!append) reset();
        if (!ok) return false;
        const uint64_t prev_offset = next_offset_;
        for (auto& elem : src) {
            const uint64_t id = id_of(elem);
            if (seen_ids_.insert(id).second) {
                on_accept(std::move(elem));
            }
        }
        next_offset_ = std::max(next_offset_, result_next_offset);
        return !append || next_offset_ > prev_offset;
    }

private:
    std::unordered_set<uint64_t> seen_ids_;
    uint64_t next_offset_ = 0;
};

} // namespace usyseek::ui
