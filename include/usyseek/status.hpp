#pragma once

#include "common.hpp"

#include <wil/resource.h>

namespace usyseek {

enum class Phase : uint32_t {
    Idle = 0,
    Starting,
    LoadingModel,
    Scanning,
    Comparing,
    Embedding,
    Cleaning,
    Done,
    Stopped,
    Error,
};

inline const wchar_t* phase_label(Phase p) noexcept {
    switch (p) {
        case Phase::Idle: return L"Idle";
        case Phase::Starting: return L"Starting";
        case Phase::LoadingModel: return L"Loading model";
        case Phase::Scanning: return L"Scanning drives";
        case Phase::Comparing: return L"Comparing";
        case Phase::Embedding: return L"Indexing active";
        case Phase::Cleaning: return L"Cleaning up";
        case Phase::Done: return L"Up to date";
        case Phase::Stopped: return L"Stopped";
        case Phase::Error: return L"Error";
    }
    return L"";
}

/// Plain-old-data snapshot of indexer progress, published through shared memory.
struct StatusData {
    uint32_t pid = 0;
    Phase phase = Phase::Idle;
    uint64_t found = 0;      // media files in scope
    uint64_t embedded = 0;   // files with an up-to-date embedding
    uint64_t todo = 0;       // files queued this session
    uint64_t processed = 0;  // files handled this session (including failures)
    uint64_t failed = 0;
    float rate = 0.0f;       // files / second
    uint32_t batch = 0;
    int64_t started_unix = 0;
    wchar_t current[520]{};
    wchar_t device[96]{};
    wchar_t message[260]{};
};

/// Seqlock over a named file mapping. One writer (the indexer), any number of readers.
class StatusBlock {
public:
    struct Shared {
        std::atomic<uint32_t> seq;
        StatusData data;
    };

    static std::optional<StatusBlock> create() {
        StatusBlock b;
        b.mapping_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), STATUS_SHM));
        if (!b.mapping_) return std::nullopt;
        if (!b.map()) return std::nullopt;
        return b;
    }

    static std::optional<StatusBlock> open() {
        StatusBlock b;
        b.mapping_.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, STATUS_SHM));
        if (!b.mapping_) return std::nullopt;
        if (!b.map(true)) return std::nullopt;
        return b;
    }

    void write(const StatusData& d) noexcept {
        auto* s = shared();
        const uint32_t v = s->seq.load(std::memory_order_relaxed);
        s->seq.store(v + 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&s->data, &d, sizeof(StatusData));
        std::atomic_thread_fence(std::memory_order_release);
        s->seq.store(v + 2, std::memory_order_release);
    }

    [[nodiscard]] StatusData read() const noexcept {
        const auto* s = shared();
        StatusData out;
        for (int i = 0; i < 64; ++i) {
            const uint32_t a = s->seq.load(std::memory_order_acquire);
            if (a & 1) {
                YieldProcessor();
                continue;
            }
            std::memcpy(&out, const_cast<const StatusData*>(&s->data), sizeof(StatusData));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s->seq.load(std::memory_order_acquire) == a) break;
        }
        return out;
    }

private:
    bool map(bool read_only = false) {
        view_.reset(MapViewOfFile(mapping_.get(), read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        return static_cast<bool>(view_);
    }
    Shared* shared() const noexcept { return static_cast<Shared*>(view_.get()); }

    wil::unique_handle mapping_;
    wil::unique_mapview_ptr<void> view_;
};

} // namespace usyseek
