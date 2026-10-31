#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// HDVK_TIMING=2: time the app spends in Eldr's entry points between two
// render pass executes (GetRenderStats, render buffer maps and allocations,
// the render pass's IsConverged, camera and other prim syncs), for the frame timing CSV
// (renderPass.cpp) to split the app's time into Eldr's and its own.
struct HdVkCallTimes {
    // kSync: meshes, lights, materials, instancers (thread time: syncs run in
    // parallel, and a mesh's includes its instancer's). After kCount: parts
    // of a mesh's sync, not counted again in app_outside_eldr_ms.
    enum Entry {
        kStats, kMap, kAllocate, kConverged, kCameraSync, kSync, kCount,
        kSyncTopology = kCount, kSyncPoints, kSyncInstancer, kSyncPrimvars, kSyncSkin, kSyncShutter, kSyncProgram,
        kSyncFlatten, kSyncTransforms, kSyncJoints, kSyncShare, kAll
    };
    std::atomic<int64_t> ns[kAll] = {};
    std::atomic<int64_t> calls[kAll] = {};
    // The wall-clock span of the kSync calls since the last execute (ns on
    // steady_clock): the share of the app's time its prim syncs take.
    std::atomic<int64_t> syncBegin{INT64_MAX}, syncEnd{0};

    static bool On()
    {
        static const bool on = [] {
            const char *level = std::getenv("HDVK_TIMING");
            return level && std::atoi(level) >= 2;
        }();
        return on;
    }
    static HdVkCallTimes &Get()
    {
        static HdVkCallTimes times;
        return times;
    }
};

// Adds the scope's time to an entry (only with HDVK_TIMING=2).
class HdVkTimedCall
{
public:
    explicit HdVkTimedCall(HdVkCallTimes::Entry e) : _entry(e), _on(HdVkCallTimes::On())
    {
        if (_on) {
            _start = std::chrono::steady_clock::now();
        }
    }
    ~HdVkTimedCall()
    {
        if (_on) {
            HdVkCallTimes &t = HdVkCallTimes::Get();
            const auto end = std::chrono::steady_clock::now();
            t.ns[_entry] += std::chrono::duration_cast<std::chrono::nanoseconds>(end - _start).count();
            ++t.calls[_entry];
            if (_entry == HdVkCallTimes::kSync) {
                using ns = std::chrono::nanoseconds;
                const int64_t b = std::chrono::duration_cast<ns>(_start.time_since_epoch()).count(),
                              e = std::chrono::duration_cast<ns>(end.time_since_epoch()).count();
                for (int64_t v = t.syncBegin; b < v && !t.syncBegin.compare_exchange_weak(v, b);) {
                }
                for (int64_t v = t.syncEnd; e > v && !t.syncEnd.compare_exchange_weak(v, e);) {
                }
            }
        }
    }

private:
    HdVkCallTimes::Entry _entry;
    bool _on;
    std::chrono::steady_clock::time_point _start;
};

// HDVK_TIMING: a line on stderr the first `times` times this call site runs
// (a startup timeline: a log reader stamps the lines as they arrive).
#define HDVK_MARK(times, ...)                                                  \
    do {                                                                       \
        static std::atomic<int> hdvkMarks{0};                                  \
        if (std::getenv("HDVK_TIMING") && hdvkMarks.fetch_add(1) < (times)) {  \
            std::fprintf(stderr, "HdVk mark: " __VA_ARGS__);                   \
            std::fputc('\n', stderr);                                          \
        }                                                                      \
    } while (0)
