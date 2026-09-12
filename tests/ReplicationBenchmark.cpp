#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationTransport.hpp"
#include "gargantuan/network/Transport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <span>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;

	class MeasuringTransport final : public IGameTransport {
	  public:
		TransportOperationResult Start(const TransportStartConfiguration &) override {
			return {.Status = TransportOperationStatus::Succeeded};
		}
		TransportOperationResult Stop(DisconnectInfo Information) override {
			return {.Status = TransportOperationStatus::Succeeded, .TerminalDisconnect = std::move(Information)};
		}
		TransportOperationResult Disconnect(ConnectionId, DisconnectInfo Information) override {
			return {.Status = TransportOperationStatus::Succeeded, .TerminalDisconnect = std::move(Information)};
		}
		TransportOperationResult Send(const NetworkMessageIntent &Message) override {
			Bytes += Message.Payload().size();
			++Messages;
			return {.Status = TransportOperationStatus::Succeeded};
		}
		std::size_t PollEvents(std::span<TransportEvent>) override {
			return 0;
		}
		std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId) const override {
			return 1200;
		}
		std::optional<NetworkStatistics> GetStatistics(ConnectionId) const override {
			return std::nullopt;
		}
		std::size_t Bytes = 0;
		std::size_t Messages = 0;
	};

	double Milliseconds(auto Start, auto End) {
		return std::chrono::duration<double, std::milli>(End - Start).count();
	}

	bool RunIncrementalScaling(std::size_t Count) {
		auto World = std::make_shared<DataModel>();
		std::shared_ptr<Folder> Target;
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetName("FixedWidthName");
			Object->SetParent(World);
			Target = Object;
		}
		ReplicationCoordinator Coordinator(World);
		auto Baseline = Coordinator.AddPeer({1, 1}, ReplicationEpoch(1));
		ReplicaApplier Applier;
		if (!Baseline.Succeeded() || !Applier.ApplyFrame(*Baseline.Frame).Succeeded()) return false;
		for (std::size_t Trial = 0; Trial <= 10; ++Trial) {
			ReplicationFrame Frame{ReplicationProtocolVersion, ReplicationMessageKind::Incremental,
				Applier.GetEpoch(), Applier.GetNextSequence()};
			if (Trial == 10) Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Target->GetObjectId()}});
			else Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Target->GetObjectId(), "Name",
				std::string(Trial % 2 == 0 ? "ChangedNameAA" : "ChangedNameBB")}});
			const auto Before = Applier.GetMetrics();
			runtime_detail::WorkSample Sample{};
			const auto Started = std::chrono::steady_clock::now();
			ReplicaApplyResult Applied;
			{
				runtime_detail::WorkCapture Capture(&Sample);
				Applied = Applier.ApplyFrame(Frame);
			}
			const auto Ended = std::chrono::steady_clock::now();
			if (!Applied.Succeeded()) { std::cerr << Applied.Message << '\n'; return false; }
			const auto &After = Applier.GetMetrics();
			auto Counter = [&](runtime_detail::WorkCounter Value) { return Sample.Counters[static_cast<std::size_t>(Value)]; };
			std::cout << "[Replication:IncrementalScaling] objects=" << Count + 1 << " trial=" << Trial
				<< " removal=" << (Trial == 10) << " operations=1 totalMs=" << Milliseconds(Started, Ended)
				<< " copyMs=" << (After.CandidateCopyNanoseconds - Before.CandidateCopyNanoseconds) / 1e6
				<< " semanticMs=" << (After.SemanticValidationNanoseconds - Before.SemanticValidationNanoseconds) / 1e6
				<< " preflightMs=" << (After.ValidationLoadNanoseconds - Before.ValidationLoadNanoseconds) / 1e6
				<< " liveMs=" << (After.LiveApplyNanoseconds - Before.LiveApplyNanoseconds) / 1e6
				<< " copied=" << Counter(runtime_detail::WorkCounter::ClientCopiedIdentities)
				<< " indexed=" << Counter(runtime_detail::WorkCounter::ClientIndexedIdentities)
				<< " preflightObjects=" << Counter(runtime_detail::WorkCounter::ClientPreflightIdentities)
				<< " receiverVisits=" << Counter(runtime_detail::WorkCounter::ClientReceiverVisits)
				<< " removalVisits=" << Counter(runtime_detail::WorkCounter::ClientRemovalVisits) << '\n';
		}
		Applier.Reset();
		World->Destroy();
		return true;
	}

	bool Run(std::size_t Count) {
		auto Game = std::make_shared<DataModel>();
		Game->SetName("BenchmarkWorld");
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Child = std::make_shared<Folder>();
			Child->SetName("Object" + std::to_string(Index));
			Child->SetParent(Game);
		}
		const ConnectionId Connection{1, 1};
		const auto StartGenerate = std::chrono::steady_clock::now();
		ReplicationCoordinator Coordinator(Game);
		auto Baseline = Coordinator.AddPeer(Connection, ReplicationEpoch(1));
		const auto EndGenerate = std::chrono::steady_clock::now();
		if (!Baseline.Succeeded()) {
			std::cerr << Baseline.Error << '\n';
			return false;
		}
		const auto StartEncode = std::chrono::steady_clock::now();
		auto Bytes = EncodeReplicationFrame(*Baseline.Frame);
		const auto EndEncode = std::chrono::steady_clock::now();
		if (!Bytes) {
			std::cerr << Bytes.error().Format() << '\n';
			return false;
		}
		const auto StartDecode = std::chrono::steady_clock::now();
		auto Decoded = DecodeReplicationFrame(*Bytes);
		const auto EndDecode = std::chrono::steady_clock::now();
		if (!Decoded) {
			std::cerr << Decoded.error().Format() << '\n';
			return false;
		}
		MeasuringTransport Transport;
		NetworkScheduler Scheduler(Transport);
		const auto Limits = NetworkLimits::NativeCeilings();
		Scheduler.RegisterConnection(Connection, Limits);
		const auto StartAdmission = std::chrono::steady_clock::now();
		auto Queued = QueueReplicationFrame(*Baseline.Frame, Connection, Limits, Scheduler);
		auto Flushed = Scheduler.Flush(Connection, SchedulerTickBudget::FromNetworkLimits(Limits));
		const auto EndAdmission = std::chrono::steady_clock::now();
		if (!Queued || !Queued->Accepted() || Flushed.Status != SchedulerFlushStatus::Drained) return false;
		ReplicaApplier Applier;
		const auto StartApply = std::chrono::steady_clock::now();
		auto Applied = Applier.ApplyFrame(*Decoded);
		const auto EndApply = std::chrono::steady_clock::now();
		if (!Applied.Succeeded()) {
			std::cerr << Applied.Message << '\n';
			return false;
		}
		std::cout << "ReplicationBaseline," << Count << ',' << Bytes->size() << ','
				  << Milliseconds(StartGenerate, EndGenerate) << ',' << Milliseconds(StartEncode, EndEncode) << ','
				  << Milliseconds(StartDecode, EndDecode) << ',' << Milliseconds(StartAdmission, EndAdmission) << ','
				  << Milliseconds(StartApply, EndApply) << '\n';
		return true;
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Error) {
		std::cerr << Error.what() << '\n';
		return 1;
	}
	std::cout << "Workload,Objects,Bytes,GenerateMs,EncodeMs,DecodeMs,SchedulerTransportMs,ApplyMs\n";
	if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--incremental-scaling") {
		for (const auto Count : {64u, 512u, 2048u, 8192u}) if (!RunIncrementalScaling(Count)) return 1;
		return 0;
	}
	if (!Run(1000)) return 1;
	if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full" && !Run(10000)) return 1;
	return 0;
}
