#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

// HDVK_TIMING=2: time the app spends in Eldr's entry points between two
// render pass executes (GetRenderStats, render buffer maps and allocations,
// the render pass's IsConverged, camera and other prim syncs), for the frame timing CSV
// (renderPass.cpp) to split the app's time into Eldr's and its own.
struct HdVkCallTimes {
    enum Entry { kStats, kMap, kAllocate, kConverged, kCameraSync, kSync, kCount };  // kSync: meshes, lights, materials, instancers
    std::atomic<int64_t> ns[kCount] = {};
    std::atomic<int64_t> calls[kCount] = {};

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
            t.ns[_entry] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                                  _start).count();
            ++t.calls[_entry];
        }
    }

private:
    HdVkCallTimes::Entry _entry;
    bool _on;
    std::chrono::steady_clock::time_point _start;
};
