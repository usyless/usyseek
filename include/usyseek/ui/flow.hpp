#pragma once

#include <algorithm>
#include <span>
#include <vector>

namespace usyseek::ui {

struct FlowItem {
    float width = 0.0f;
    float height = 0.0f;
    bool grow = false;
};

struct FlowPlacement {
    float x;
    float y;
    float width;
    int line;
    bool oversize;
};

struct FlowResult {
    std::vector<FlowPlacement> placements;
    float height = 0.0f;
};

/// Places items left to right, starting a new line when the next item no longer fits in `avail`.
/// An item wider than `avail` gets a line of its own, is clamped to `avail` and flagged `oversize`.
/// Items marked `grow` share the unused width of their line equally.
inline FlowResult flow_layout(std::span<const FlowItem> items, float gap_x, float gap_y, float avail) {
    constexpr float tolerance = 0.001f;
    FlowResult result;
    result.placements.reserve(items.size());

    size_t line_begin = 0;
    float y = 0.0f;
    int line = 0;
    while (line_begin < items.size()) {
        size_t line_end = line_begin;
        float used = 0.0f;
        while (line_end < items.size()) {
            const float w = std::min(items[line_end].width, avail);
            const float next = used + (line_end > line_begin ? gap_x : 0.0f) + w;
            if (line_end > line_begin && (next > avail + tolerance || items[line_end].width > avail + tolerance)) break;
            used = next;
            ++line_end;
            if (items[line_end - 1].width > avail + tolerance) break;
        }

        int growers = 0;
        float line_h = 0.0f;
        for (size_t i = line_begin; i < line_end; ++i) {
            if (items[i].grow) ++growers;
            line_h = std::max(line_h, items[i].height);
        }
        const float extra = growers > 0 ? std::max(0.0f, avail - used) / static_cast<float>(growers) : 0.0f;

        float x = 0.0f;
        for (size_t i = line_begin; i < line_end; ++i) {
            const float w = std::min(items[i].width, avail) + (items[i].grow ? extra : 0.0f);
            result.placements.emplace_back(x, y, w, line, items[i].width > avail + tolerance);
            x += w + gap_x;
        }

        y += line_h + gap_y;
        ++line;
        line_begin = line_end;
    }

    result.height = items.empty() ? 0.0f : y - gap_y;
    return result;
}

} // namespace usyseek::ui
