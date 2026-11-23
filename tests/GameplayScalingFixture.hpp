#pragma once

#include "../src/runtime/PublicationLatencyDiagnostics.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"
#include "../src/runtime/RuntimeWorkTrace.hpp"
#include "WorkloadTimingEvidence.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace gargantuan::test_detail {
	struct GameplayScalingOptions {
		std::size_t Ticks;
		bool Profile;
		bool DeadlinePacing = false;
	};

	class GameplayScalingCapture final {
		using WorkSample = runtime_detail::WorkSample;
		using ConnectionId = network::ConnectionId;
		using Clock = std::chrono::steady_clock;

		static constexpr std::size_t PhaseStride = 8;
		static constexpr std::uint64_t PacketStride = 32;
		static constexpr std::size_t MaximumPackets = 262144;
		struct Frame {
			std::uint64_t Tick = 0, Interval = 0, Wait = 0, Pump = 0;
			std::uint64_t ServerPoll = 0, ServerEngine = 0, ServerSession = 0;
			std::uint64_t ClientPoll = 0, ClientEngine = 0, ClientSession = 0;
			std::uint64_t ServerAllocations = 0, ServerBytes = 0;
			std::uint64_t DeadlineLateness = 0;
			bool MissedDeadlineBeforeWait = false;
			std::uint64_t Poses = 0, PoseBytes = 0, Produced = 0, Admitted = 0, Consumed = 0, Coalesced = 0;
			std::uint64_t Applied = 0, StructuralApply = 0, MaximumClientStructuralApply = 0;
		};
		struct Phases {
			WorkSample Server{}, Client{};
		};
		struct Packet {
			const char *Stage;
			ConnectionId Connection;
			ObjectId Object;
			std::uint64_t Tick, Order, Epoch, At;
			std::uint32_t Endpoint;
			std::uint8_t Family;
		};
		std::vector<Frame> Frames;
		std::vector<Phases> Samples;
		std::vector<Packet> Packets;
#if defined(GARGANTUAN_WORK_TRACE)
		std::unique_ptr<runtime_detail::WorkTrace> Trace = runtime_detail::WorkTrace::FromEnvironment();
		std::unique_ptr<Phases> TracePhases = Trace ? std::make_unique<Phases>() : nullptr;
		bool TraceFrame = false;
#endif
		runtime_detail::PublicationLatencySink Sink;
		runtime_detail::PublicationLatencySink *PreviousSink = nullptr;
		WorkloadCpuSample CpuBegin{}, CpuEnd{};
		Clock::time_point Started{}, FrameStarted{}, Ended{};
		std::uint64_t LostPackets = 0;
		std::size_t Index = 0;
		std::uint32_t Endpoint = 0;
		bool Recording = false;

		static std::uint64_t Read(std::span<const std::byte> Data, std::size_t Offset, std::size_t Size) {
			std::uint64_t Value = 0;
			for (std::size_t Byte = 0; Byte < Size; ++Byte)
				Value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(Data[Offset + Byte])) << (8 * Byte);
			return Value;
		}

		void Append(Packet Value) noexcept {
			if (Packets.size() == MaximumPackets) ++LostPackets;
			else Packets.push_back(Value);
		}

		static void RecordPacket(void *Context, const char *Stage, ConnectionId Connection,
			std::span<const std::byte> Data, std::uint64_t At) noexcept {
			const bool Input = Data.size() == 52 && std::memcmp(Data.data(), "GPIN", 4) == 0;
			const bool Pose = Data.size() == 60 && std::memcmp(Data.data(), "GPOS", 4) == 0;
			if ((!Input && !Pose) || Data[6] != std::byte{2}) return;
			const auto Order = Read(Data, 24, 8);
			if (Order % PacketStride != 0) return;
			auto &Capture = *static_cast<GameplayScalingCapture *>(Context);
			Capture.Append({
				Stage, Connection, {static_cast<std::uint32_t>(Read(Data, 8, 4)), static_cast<std::uint32_t>(Read(Data, 12, 4))},
				Input ? Read(Data, 32, 8) : Order, Order, Read(Data, 16, 8), At, Capture.Endpoint,
				static_cast<std::uint8_t>(Input ? 1 : 2)
			});
		}

		static void RecordEvent(void *Context, runtime_detail::PublicationLatencyRecord Event) noexcept {
			if (Event.Sequence % PacketStride != 0) return;
			const bool Input = std::strcmp(Event.Stage, "PossessionConsumed") == 0;
			if (!Input && std::strcmp(Event.Stage, "PoseApplied") != 0) return;
			auto &Capture = *static_cast<GameplayScalingCapture *>(Context);
			Capture.Append({Event.Stage, Event.Connection, Event.Object, Event.Tick, Event.Sequence, Event.Epoch,
				Event.Nanoseconds, Capture.Endpoint, static_cast<std::uint8_t>(Input ? 1 : 2)});
		}

		static std::uint64_t Elapsed(Clock::time_point Begin, Clock::time_point End) {
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(End - Begin).count());
		}

	public:
		enum class Phase { Wait, Pump, ServerPoll, ServerEngine, ServerSession, ClientPoll, ClientEngine, ClientSession };

		explicit GameplayScalingCapture(const GameplayScalingOptions &Options)
			: Frames(Options.Ticks), Samples(Options.Profile ? (Options.Ticks + PhaseStride - 1) / PhaseStride : 0),
			  Sink{.Context = this, .Selected = [](void *, ConnectionId) noexcept { return true; },
				  .Record = RecordEvent, .Packet = RecordPacket, .CapturePossession = true} {
			if (Options.Profile) Packets.reserve(MaximumPackets);
		}

		~GameplayScalingCapture() { Stop(); }
		GameplayScalingCapture(const GameplayScalingCapture &) = delete;
		GameplayScalingCapture &operator=(const GameplayScalingCapture &) = delete;

		void Start() {
			CpuBegin = CaptureWorkloadCpu();
			Started = FrameStarted = Clock::now();
			PreviousSink = runtime_detail::ActivePublicationLatency;
			if (!Samples.empty()) runtime_detail::ActivePublicationLatency = &Sink;
			Recording = true;
		}

		void Stop() {
			if (!Recording) return;
			Ended = Clock::now();
			CpuEnd = CaptureWorkloadCpu();
			runtime_detail::ActivePublicationLatency = PreviousSink;
			Recording = false;
		}

		void BeginFrame(std::size_t FrameIndex, std::uint64_t Tick) {
			Index = FrameIndex;
			const auto Now = Clock::now();
			Frames[Index].Tick = Tick;
			Frames[Index].Interval = Elapsed(FrameStarted, Now);
			FrameStarted = Now;
#if defined(GARGANTUAN_WORK_TRACE)
			TraceFrame = Trace && Trace->SelectFrame(FrameIndex, Tick);
			if (TraceFrame) *TracePhases = Phases{};
#endif
		}

		void SetEndpoint(std::uint32_t Value) {
			Endpoint = Value;
#if defined(GARGANTUAN_WORK_TRACE)
			if (TraceFrame) runtime_detail::WorkTrace::Region::SetCurrentLane(Value);
#endif
		}

		void RecordDeadline(Clock::time_point Deadline, Clock::time_point BeforeWait) {
			auto &Row = Frames[Index];
			Row.MissedDeadlineBeforeWait = BeforeWait > Deadline;
			Row.DeadlineLateness = Elapsed(Deadline, std::max(Deadline, Clock::now()));
		}

		void RecordWorkload(std::uint64_t Poses, std::uint64_t PoseBytes, std::uint64_t Produced,
			std::uint64_t Admitted, std::uint64_t Consumed, std::uint64_t Coalesced, std::uint64_t Applied,
			std::uint64_t StructuralApply, std::uint64_t MaximumClientStructuralApply) {
			auto &Row = Frames[Index];
			Row.Poses = Poses;
			Row.PoseBytes = PoseBytes;
			Row.Produced = Produced;
			Row.Admitted = Admitted;
			Row.Consumed = Consumed;
			Row.Coalesced = Coalesced;
			Row.Applied = Applied;
			Row.StructuralApply = StructuralApply;
			Row.MaximumClientStructuralApply = MaximumClientStructuralApply;
#if defined(GARGANTUAN_WORK_TRACE)
			if (TraceFrame) {
				const auto At = runtime_detail::WorkTimestamp();
				Trace->RecordFrame(Row.Tick, 0, WorkloadTimestamp(FrameStarted), Row.Interval,
					Row.DeadlineLateness, Row.MissedDeadlineBeforeWait);
				const auto *PhasesAt = !Samples.empty() && Index % PhaseStride == 0 ? &Samples[Index / PhaseStride] : TracePhases.get();
				Trace->RecordSample(Row.Tick, 0, At, PhasesAt->Server);
				Trace->RecordSample(Row.Tick, 1, At, PhasesAt->Client);
			}
#endif
		}

		template <typename Operation>
		void Measure(Phase Work, Operation &&Run, runtime_detail::WorkAllocationReader Allocations,
			runtime_detail::WorkAllocationReader Bytes) {
			if (!Recording) {
				Run();
				return;
			}
			static constexpr std::array Fields{
				&Frame::Wait, &Frame::Pump, &Frame::ServerPoll, &Frame::ServerEngine, &Frame::ServerSession,
				&Frame::ClientPoll, &Frame::ClientEngine, &Frame::ClientSession
			};
			const bool Server = Work == Phase::ServerPoll || Work == Phase::ServerEngine || Work == Phase::ServerSession;
			WorkSample *Sample = nullptr;
			if (!Samples.empty() && Index % PhaseStride == 0) {
				auto &Row = Samples[Index / PhaseStride];
				Sample = Server ? &Row.Server : &Row.Client;
			}
			Endpoint = 0;
			const auto BeforeCount = Allocations();
			const auto BeforeBytes = Bytes();
			const auto Before = Clock::now();
			{
#if defined(GARGANTUAN_WORK_TRACE)
				if (TraceFrame) {
					if (!Sample) Sample = Server ? &TracePhases->Server : &TracePhases->Client;
					runtime_detail::WorkTrace::Region Region(Trace.get(), Frames[Index].Tick, Server ? 0 : 1);
					runtime_detail::WorkCapture Capture(Sample, Allocations, Bytes);
					Run();
				} else {
#endif
				runtime_detail::WorkCapture Capture(Sample);
				Run();
#if defined(GARGANTUAN_WORK_TRACE)
				}
#endif
			}
			auto &Row = Frames[Index];
			Row.*Fields[static_cast<std::size_t>(Work)] += Elapsed(Before, Clock::now());
			if (Server) {
				Row.ServerAllocations += Allocations() - BeforeCount;
				Row.ServerBytes += Bytes() - BeforeBytes;
			}
		}

		void Print(std::ostream &Output) const {
#if defined(GARGANTUAN_WORK_TRACE)
			if (Trace) Trace->Write();
#endif
			const auto ExportStarted = Clock::now();
			for (std::size_t Step = 0; Step < Frames.size(); ++Step) {
				const auto &Row = Frames[Step];
				Output << "[Gameplay:Tick] " << Row.Tick << ',' << Row.Interval << ',' << Row.Wait << ',' << Row.Pump << ','
					<< Row.ServerPoll << ',' << Row.ServerEngine << ',' << Row.ServerSession << ','
					<< Row.ClientPoll << ',' << Row.ClientEngine << ',' << Row.ClientSession << ','
					<< Row.ServerAllocations << ',' << Row.ServerBytes << '\n';
				Output << "[Gameplay:FrameWork] " << Row.Tick << ',' << Row.DeadlineLateness << ','
					<< Row.MissedDeadlineBeforeWait << ',' << Row.Poses << ',' << Row.PoseBytes << ',' << Row.Produced
					<< ',' << Row.Admitted << ',' << Row.Consumed << ',' << Row.Coalesced << ',' << Row.Applied << ','
					<< Row.StructuralApply << ',' << Row.MaximumClientStructuralApply << '\n';
				if (Samples.empty() || Step % PhaseStride != 0) continue;
				const auto PrintPhases = [&](const WorkSample &Work, const char *Side) {
					for (std::size_t PhaseIndex = 0; PhaseIndex < Work.size(); ++PhaseIndex) {
						const auto &Value = Work[PhaseIndex];
						if (Value.Calls == 0 && Value.Units == 0) continue;
						Output << "[Gameplay:Phase] " << Row.Tick << ',' << Side << ',' << runtime_detail::WorkPhaseNames[PhaseIndex]
							<< ',' << Value.Nanoseconds << ',' << Value.ExclusiveNanoseconds << ',' << Value.Calls
							<< ',' << Value.Units << ',' << Value.Retained << ',' << Value.Rejected << '\n';
					}
					for (std::size_t Producer = 0; Producer < Work.Producers.size(); ++Producer) {
						const auto &Value = Work.Producers[Producer];
						if (Value.Enqueued == 0 && Value.Serviced == 0) continue;
						Output << "[Gameplay:Queue] " << Row.Tick << ',' << Side << ','
							<< runtime_detail::WorkProducerNames[Producer] << ',' << Value.Enqueued << ','
							<< Value.Serviced << ',' << Value.QueueDepthMaximum << ',' << Value.ServiceAgeMaximumNanoseconds << '\n';
					}
				};
				const auto &PhasesAt = Samples[Step / PhaseStride];
				PrintPhases(PhasesAt.Server, "server");
				PrintPhases(PhasesAt.Client, "clients");
			}
			for (const auto &Row : Packets)
				Output << "[Gameplay:Packet] " << Row.Stage << ',' << static_cast<unsigned>(Row.Family) << ','
					<< Row.Endpoint << ',' << Row.Connection.Slot << ',' << Row.Connection.Generation << ','
					<< Row.Object.Slot << ',' << Row.Object.Generation << ',' << Row.Tick << ',' << Row.Order << ','
					<< Row.Epoch << ',' << (Row.At - WorkloadTimestamp(Started)) << '\n';
			Output.flush();
			Output << "[Gameplay:Capture] wall_ns=" << Elapsed(Started, Ended)
				<< " export_ns=" << Elapsed(ExportStarted, Clock::now())
				<< " phase_samples=" << Samples.size() << " packet_samples=" << Packets.size()
				<< " lost_packets=" << LostPackets << " buffer_bytes="
				<< Frames.capacity() * sizeof(Frame) + Samples.capacity() * sizeof(Phases) + Packets.capacity() * sizeof(Packet);
			if (const auto Cpu = WorkloadTimingEvidence::CpuDelta(
				CpuBegin.Thread100ns, CpuEnd.Thread100ns, CpuBegin.ThreadValid, CpuEnd.ThreadValid))
				Output << " main_cpu_ms=" << *Cpu;
			if (const auto Cpu = WorkloadTimingEvidence::CpuDelta(
				CpuBegin.Process100ns, CpuEnd.Process100ns, CpuBegin.ProcessValid, CpuEnd.ProcessValid))
				Output << " process_cpu_ms=" << *Cpu;
#if defined(_WIN32)
			PROCESS_MEMORY_COUNTERS_EX Memory{};
			Memory.cb = sizeof(Memory);
			if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Memory), sizeof(Memory)))
				Output << " rss_bytes=" << Memory.WorkingSetSize << " peak_rss_bytes=" << Memory.PeakWorkingSetSize
					<< " private_bytes=" << Memory.PrivateUsage;
#elif defined(__linux__)
			rusage Usage{};
			if (getrusage(RUSAGE_SELF, &Usage) == 0)
				Output << " peak_rss_bytes=" << static_cast<std::uint64_t>(Usage.ru_maxrss) * 1024;
#endif
			Output << '\n';
		}
	};
}
