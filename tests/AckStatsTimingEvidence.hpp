#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <steam/steamnetworkingsockets.h>
#include "../cmake/gns/ServiceTimingDiagnostics.hpp"
#include "WorkloadTimingEvidence.hpp"

// Fixture-only opt-in observation. Never used by any timer or F1 predicate.
namespace AckStatsTimingEvidence {
inline bool Enabled(bool Cycle = false) noexcept {
#if defined(_WIN32)
    char Value[2]{};
    return GetEnvironmentVariableA(Cycle ? "GARGANTUAN_SCHEDULER_ACK_CYCLE" :
        "GARGANTUAN_SCHEDULER_ACK_STATS", Value, 2) == 1 && Value[0] == '1';
#else
    (void)Cycle;
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
// The existing child-stream limit is 32 MiB. At most 512 printed bytes per
// phase record permits 65,536 retained records; overflow stays diagnostic.
inline constexpr std::size_t ServiceTimingRecordLimit = (32 * 1024 * 1024) / 512;
struct ServiceTimingBuffer {
    std::array<SteamNetworkingSocketsLib::GargantuanServiceTimingRecord, ServiceTimingRecordLimit> Records{};
    std::atomic<std::uint64_t> Count{0};
    std::atomic<bool> Overflow{false};
    static void Record(void *Context, const SteamNetworkingSocketsLib::GargantuanServiceTimingRecord &Value) noexcept {
        auto &Buffer = *static_cast<ServiceTimingBuffer *>(Context);
        const auto Index = Buffer.Count.fetch_add(1, std::memory_order_relaxed);
        if (Index >= Buffer.Records.size()) { Buffer.Overflow.store(true, std::memory_order_release); return; }
        Buffer.Records[static_cast<std::size_t>(Index)] = Value;
    }
};
inline const char *ArmName(unsigned Arm) noexcept {
    return Arm == 1 ? "ackstats-control" : (Arm == 2 ? "ackstats-prompt" :
        (Arm == 3 ? "ackcycle-funded" : "INVALID"));
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
    bool Active;
    bool Cycle;
    std::unique_ptr<ServiceTimingBuffer> Timing;
    SteamNetworkingSocketsLib::GargantuanServiceTimingSink TimingSink{};
    gargantuan::test_detail::WorkloadClockAnchor TimingClock{};
    explicit Session(bool CycleCase = false) : Active(Enabled(CycleCase)), Cycle(CycleCase) {
        if (!Active) return;
        if (Cycle) {
            Timing = std::make_unique<ServiceTimingBuffer>();
            TimingClock = gargantuan::test_detail::WorkloadClockAnchor::Capture();
            TimingSink = {Timing.get(), ServiceTimingBuffer::Record};
            if (!SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(&TimingSink))
                throw std::runtime_error("ACK cycle timing sink already owned");
        }
        ThreadCount.store(0);
        for (auto &Entry : ServiceStarts) Entry.Ready.store(false);
        SteamNetworkingSockets_SetServiceThreadInitCallback(ServiceThreadStarted);
    }
    ~Session() {
        if (!Active) return;
        // Observe owns/destroys every pair before Run's Session unwinds.
        const bool SinkCleared = !Cycle || SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(nullptr);
        SteamNetworkingSockets_SetServiceThreadInitCallback(nullptr);
        const auto Count = ThreadCount.load(std::memory_order_acquire);
        bool Valid = Count > 0 && Count <= ServiceStarts.size();
        for (unsigned Index = 0; Index < Count && Index < ServiceStarts.size(); ++Index) {
            const auto &Entry = ServiceStarts[Index];
            if (!Entry.Ready.load(std::memory_order_acquire)) { Valid = false; continue; }
            const auto &Clock = Entry.Clock;
            Valid = Valid && Clock.NativeValid && (Cycle ? Entry.Arm == 3 : Entry.Arm >= 1 && Entry.Arm <= 2);
            std::cout << (Cycle ? "[Qualification:AckCycleServiceThread] case=" :
                "[Qualification:AckStatsServiceThread] case=") << ArmName(Entry.Arm)
                << " sequence=" << Index << " pid=" << Clock.Process << " native_tid=" << Clock.Thread
                << " native_valid=" << Clock.NativeValid << " qpc_before=" << Clock.QpcBefore
                << " qpc_after=" << Clock.QpcAfter << " qpc_frequency=" << Clock.QpcFrequency << '\n';
        }
        std::cout << (Cycle ? "[Qualification:AckCycleDiagnostic] thread_count=" :
            "[Qualification:AckStatsDiagnostic] thread_count=") << Count << " valid=" << Valid << '\n';
        if (Cycle) {
            const auto TimingCount = Timing->Count.load(std::memory_order_acquire);
            const bool Overflow = Timing->Overflow.load(std::memory_order_acquire);
            bool TimingValid = SinkCleared && TimingClock.NativeValid && TimingCount > 0 &&
                TimingCount <= Timing->Records.size() && !Overflow;
            for (std::size_t Index = 0; Index < std::min<std::uint64_t>(TimingCount, Timing->Records.size()); ++Index) {
                const auto &Entry = Timing->Records[Index];
                TimingValid = TimingValid && Entry.ClockValid && Entry.BeginQpc > 0 && Entry.EndQpc >= Entry.BeginQpc;
                std::cout << "[Qualification:AckCycleServiceTiming] sequence=" << Index
                    << " phase=" << static_cast<unsigned>(Entry.Phase) << " pid=" << TimingClock.Process
                    << " native_tid=" << Entry.ThreadId << " clock_valid=" << Entry.ClockValid
                    << " qpc_begin=" << Entry.BeginQpc << " qpc_end=" << Entry.EndQpc
                    << " qpc_frequency=" << TimingClock.QpcFrequency << " count=" << Entry.Count
                    << " bytes=" << Entry.Bytes << " detail=" << Entry.Detail << " result=" << Entry.Result
                    << " error=" << Entry.Error << " flags=" << Entry.Flags << '\n';
            }
            std::cout << "[Qualification:AckCycleTimingSummary] records=" << TimingCount
                << " retained=" << std::min<std::uint64_t>(TimingCount, Timing->Records.size())
                << " capacity=" << Timing->Records.size() << " overflow=" << Overflow
                << " sink_cleared=" << SinkCleared << " valid=" << TimingValid << '\n';
        }
    }
};
struct Arm {
    bool Active;
    unsigned Identity;
    bool Cycle;
    explicit Arm(bool Prompt, bool CycleCase = false) : Active(Enabled(CycleCase)),
        Identity(CycleCase ? 3u : (Prompt ? 2u : 1u)), Cycle(CycleCase) {
        if (Active) {
            CurrentArm.store(Identity, std::memory_order_release);
            gargantuan::test_detail::WorkloadClockAnchor::Capture().Print(std::cout, ArmName(Identity),
                Cycle ? "ACK_CYCLE_FUNDED" : "ACK_STATS", "BEGIN");
        }
    }
    ~Arm() {
        if (Active) gargantuan::test_detail::WorkloadClockAnchor::Capture().Print(std::cout, ArmName(Identity),
            Cycle ? "ACK_CYCLE_FUNDED" : "ACK_STATS", "END");
    }
};

template<class Read, class Snapshot>
bool ReadNativeSnapshot(const char *Stage, const char *Side, bool Prompt, std::uint64_t Token, Snapshot &Value,
    Read &&ReadValue, bool Cycle = false) {
    if (!Enabled(Cycle)) return ReadValue();
    const auto Before = gargantuan::test_detail::WorkloadClockAnchor::Capture();
    const bool Available = ReadValue();
    const auto After = gargantuan::test_detail::WorkloadClockAnchor::Capture();
    std::cout << (Cycle ? "[Qualification:AckCycleNativeSnapshot] case=" :
        "[Qualification:AckStatsNativeSnapshot] case=") << ArmName(Cycle ? 3u : (Prompt ? 2u : 1u))
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
