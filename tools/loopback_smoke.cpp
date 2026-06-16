// Smoke test for per-process WASAPI loopback (tools/loopback_smoke.cpp).
// Captures ~1.5s from a target PID and reports whether process isolation was
// achieved (perProcess=1) vs. fell back to full-system capture (perProcess=0).
//
// Build+run handled by the PowerShell snippet in the chat; not part of CMake.
#include "SyncComms/WasapiCapture.h"

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#pragma comment(lib, "mfplat.lib")

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: loopback_smoke <pid>\n"); return 2; }
    const uint32_t pid = static_cast<uint32_t>(std::atoi(argv[1]));

    // Intentionally NO MFStartup here — WasapiCapture does its own, so this
    // proves it's self-contained (the real app's capture thread doesn't call it).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    SyncComms::WasapiCapture cap;
    std::atomic<uint64_t> frames{0};
    std::atomic<double>   peak{0.0};

    const bool started = cap.Start(pid, 48000, 2,
        [&](const float* data, uint32_t fc, int ch) {
            frames += fc;
            double p = 0.0;
            const uint32_t n = fc * static_cast<uint32_t>(ch);
            for (uint32_t i = 0; i < n; ++i) {
                const double a = std::fabs(data[i]);
                if (a > p) p = a;
            }
            double cur = peak.load();
            while (p > cur && !peak.compare_exchange_weak(cur, p)) {}
        });

    std::printf("pid=%u start=%d perProcess=%d rate=%d ch=%d\n",
                pid, started ? 1 : 0, cap.IsPerProcessActive() ? 1 : 0,
                cap.GetActualSampleRate(), cap.GetActualChannels());

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    cap.Stop();

    std::printf("frames=%llu peak=%.4f  (peak>0 means real audio was playing; "
                "frames>0 alone confirms the stream is live)\n",
                static_cast<unsigned long long>(frames.load()), peak.load());

    CoUninitialize();
    return started ? 0 : 1;
}
