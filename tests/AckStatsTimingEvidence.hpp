#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <steam/steamnetworkingsockets.h>
#include "WorkloadTimingEvidence.hpp"

// Fixture-only opt-in observation. Never used by any timer or F1 predicate.
namespace AckStatsTimingEvidence {
inline bool Enabled() noexcept {
#if defined(_WIN32)
    char Value[2]{};
    return GetEnvironmentVariableA("GARGANTUAN_SCHEDULER_ACK_STATS", Value, 2) == 1 && Value[0] == '1';
#else
    return false;
#endif
}
inline std::atomic<unsigned> CurrentArm{0}, ThreadCount{0};
struct ServiceStart {
    gargantuan::test_detail::WorkloadClockAnchor Clock;
    unsigned Arm = 0;
    std::atomic<bool> Ready{false};
};
inline std::array<ServiceStart, 16> ServiceStarts;
inline const char *ArmName(unsigned Arm) noexcept {
    return Arm == 1 ? "ackstats-control" : (Arm == 2 ? "ackstats-prompt" : "INVALID");
}
inline void ServiceThreadStarted() noexcept {
    const auto Index = ThreadCount.fetch_add(1, std::memory_order_relaxed);
    if (Index >= ServiceStarts.size()) return;
    auto &Entry = ServiceStarts[Index];
    Entry.Clock = gargantuan::test_detail::WorkloadClockAnchor::Capture();
    Entry.Arm = CurrentArm.load(std::memory_order_acquire);
    Entry.Ready.store(true, std::memory_order_release);
}
struct Session {
    bool Active = Enabled();
    Session() {
        if (!Active) return;
        ThreadCount.store(0);
        for (auto &Entry : ServiceStarts) Entry.Ready.store(false);
        SteamNetworkingSockets_SetServiceThreadInitCallback(ServiceThreadStarted);
    }
    ~Session() {
        if (!Active) return;
        // Observe owns/destroys every pair before Run's Session unwinds.
        SteamNetworkingSockets_SetServiceThreadInitCallback(nullptr);
        const auto Count = ThreadCount.load(std::memory_order_acquire);
        bool Valid = Count > 0 && Count <= ServiceStarts.size();
        for (unsigned Index = 0; Index < Count && Index < ServiceStarts.size(); ++Index) {
            const auto &Entry = ServiceStarts[Index];
            if (!Entry.Ready.load(std::memory_order_acquire)) { Valid = false; continue; }
            const auto &Clock = Entry.Clock;
            Valid = Valid && Clock.NativeValid && Entry.Arm >= 1 && Entry.Arm <= 2;
            std::cout << "[Qualification:AckStatsServiceThread] case=" << ArmName(Entry.Arm)
                << " sequence=" << Index << " pid=" << Clock.Process << " native_tid=" << Clock.Thread
                << " native_valid=" << Clock.NativeValid << " qpc_before=" << Clock.QpcBefore
                << " qpc_after=" << Clock.QpcAfter << " qpc_frequency=" << Clock.QpcFrequency << '\n';
        }
        std::cout << "[Qualification:AckStatsDiagnostic] thread_count=" << Count << " valid=" << Valid << '\n';
    }
};
struct Arm {
    bool Active = Enabled();
    unsigned Identity;
    explicit Arm(bool Prompt) : Identity(Prompt ? 2u : 1u) {
        if (Active) {
            CurrentArm.store(Identity, std::memory_order_release);
            gargantuan::test_detail::WorkloadClockAnchor::Capture().Print(std::cout, ArmName(Identity), "ACK_STATS", "BEGIN");
        }
    }
    ~Arm() {
        if (Active) gargantuan::test_detail::WorkloadClockAnchor::Capture().Print(std::cout, ArmName(Identity), "ACK_STATS", "END");
    }
};

template<class Read, class Snapshot>
bool ReadNativeSnapshot(const char *Stage, const char *Side, bool Prompt, std::uint64_t Token, Snapshot &Value, Read &&ReadValue) {
    if (!Enabled()) return ReadValue();
    const auto Before = gargantuan::test_detail::WorkloadClockAnchor::Capture();
    const bool Available = ReadValue();
    const auto After = gargantuan::test_detail::WorkloadClockAnchor::Capture();
    std::cout << "[Qualification:AckStatsNativeSnapshot] case=" << ArmName(Prompt ? 2u : 1u)
        << " stage=" << Stage << " side=" << Side << " token=" << Token << " available=" << Available
        << " native_us=" << (Available ? Value.NativeSnapshotNow : 0)
        << " pid=" << Before.Process << " native_tid=" << Before.Thread
        << " native_valid=" << (Before.NativeValid && After.NativeValid)
        << " qpc_before=" << Before.QpcBefore << " qpc_after=" << After.QpcAfter
        << " qpc_frequency=" << Before.QpcFrequency << '\n';
    // Native GNS clock can change its offset after long pauses. This brackets
    // only this returned snapshot; never infer a global native-us/QPC offset.
    return Available;
}
}
