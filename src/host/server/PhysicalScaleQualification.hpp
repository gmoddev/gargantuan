#pragma once

#include "host/common/FarmServerTickEvidence.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/WireValue.hpp"
#include "../../network/GameSessionTestAccess.hpp"
#include "FarmAdmissionEvidence.hpp"
#include "FarmRecoveryEvidence.hpp"
#include "../../runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan::host {
	namespace detail {
		[[nodiscard]] inline std::optional<std::uint32_t> DecodePhysicalScaleCounter(
			const std::optional<WireValue> &Value) {
			if (!Value) return std::nullopt;
			if (const auto *Integer = std::get_if<int>(&*Value)) {
				if (*Integer < 0) return std::nullopt;
				return static_cast<std::uint32_t>(*Integer);
			}
			if (const auto *Number = std::get_if<double>(&*Value)) {
				if (!std::isfinite(*Number) || *Number < 0 ||
					*Number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
					std::trunc(*Number) != *Number) return std::nullopt;
				return static_cast<std::uint32_t>(*Number);
			}
			return std::nullopt;
		}

		[[nodiscard]] inline bool HasPhysicalScaleTerminalConvergence(
			const network::GameSessionMetrics &Metrics) {
			const auto &Admission = Metrics.ReliableAdmission;
			return Metrics.ReadyPeers == 32 && Admission.TerminalReleasedBytes == 0 &&
				Admission.AcceptedBytes == Admission.VerifiedAttributedRetirement &&
				Admission.OutstandingBytes == 0 && Admission.ActiveDrainGrants == 0 &&
				Metrics.StructuralFeedbackPeersObserved == 32 &&
				Metrics.NativeQueuedReliablePeersObserved == 32 &&
				Metrics.SchedulerQueuedReliableBytes == 0 && Metrics.NativeQueuedReliableBytes == 0 &&
				Metrics.StructuralPendingEnters == Metrics.StructuralPendingLeaves &&
				Metrics.StructuralActivePeers == 0 && Metrics.MaterializationBacklog == 0 &&
				Metrics.JournalBacklogRecords == 0;
		}

		// The canonical 3L recovery curve uses complete-message W_i at cessation,
		// not raw journal history or a fixed 20.4705-second convergence deadline.
		[[nodiscard]] inline std::optional<std::uint64_t> RecoveryConvergenceBoundMicroseconds(
			std::span<const std::uint64_t> WorkByPeer) {
			const auto Profile = network::ReliableServiceProfile::PooledService().Pooled;
			if (WorkByPeer.size() != 32) return {};
			const auto PeerRate = std::min(Profile.PeerCreditRate, Profile.StructuralPool / WorkByPeer.size());
			const auto PoolRate = std::min(Profile.GlobalCreditRate, Profile.StructuralPool);
			if (!PeerRate || !PoolRate) return {};
			auto ServiceMicroseconds = [](std::uint64_t Bytes, std::uint64_t Rate) {
				return Bytes / Rate * 1'000'000 +
					((Bytes % Rate) * 1'000'000 + Rate - 1) / Rate;
			};
			std::uint64_t Total = 0, MaximumPeerService = 0;
			for (const auto Work : WorkByPeer) {
				if (Work > 40ULL * 1024 * 1024 * 1024 ||
					Total > 768ULL * 1024 * 1024 * 1024 - Work) return {};
				Total += Work;
				MaximumPeerService = std::max(MaximumPeerService, ServiceMicroseconds(Work, PeerRate));
			}
			return 20'000'000 + std::max(250'000 + 220'500 + MaximumPeerService,
				ServiceMicroseconds(Total, PoolRate));
		}
	}

	// Trusted, bounded host-only controller for the accepted 32-client recipient
	// workload. The package's server script relays the authoritative DataModel
	// ScalePhase attribute to the single producer through ScalePhaseControl and
	// records all client phase ACKs and the producer completion ACK on CharacterControl.
	// This class neither injects relevance/Desired state nor bypasses admission.
	class PhysicalScaleQualification final {
	  public:
		struct ServerTickTiming {
			std::uint64_t PollMicroseconds = 0;
			std::uint64_t EngineMicroseconds = 0;
			std::uint64_t SessionMicroseconds = 0;
			std::uint64_t SessionIncrementalMicroseconds = 0;
			std::uint64_t SessionBuildMicroseconds = 0;
			std::uint64_t SessionEncodeMicroseconds = 0;
			std::uint64_t SessionEncodeRetries = 0;
			std::uint64_t PreQualificationMicroseconds = 0;
		};
		static constexpr std::size_t PeerCount = 32;
		static constexpr std::size_t ActiveCharacters = 8;
		static constexpr std::size_t NeighborhoodSize = 8;
		static constexpr std::size_t ContentObjects = 512;
		static constexpr std::uint64_t ContentPayloadBytes = 273'032;
		static constexpr std::string_view ContentKey = "workspace/scale";
		static constexpr std::string_view ContentRootName = "ContentScaleRegion";

		PhysicalScaleQualification(Engine &RuntimeValue, network::GameSession &SessionValue, std::string RunIdValue,
			bool RecoveryWorkloadValue = false, detail::FarmServerTickEvidence *TickEvidenceValue = nullptr)
			: Runtime(RuntimeValue), Session(SessionValue), RunId(std::move(RunIdValue)),
				RecoveryWorkload(RecoveryWorkloadValue), TickEvidence(TickEvidenceValue) {
			if (RunId.empty() || !Runtime.Content || !Runtime.DataModel || !Runtime.CharacterControl)
				throw std::invalid_argument("physical scale qualification requires a content-backed server runtime");
		}
		void AttachAdmissionEvidence(detail::FarmAdmissionEvidence &Evidence) { AdmissionEvidence = &Evidence; }
		~PhysicalScaleQualification() {
			try { DumpRecoveryEvidence(); }
			catch (const std::exception &Error) {
				std::cerr << "[Qualification:Recovery] evidence_write_failed=" << Error.what() << '\n';
			}
		}
		void DumpRecoveryEvidence() const {
			if (!AdmissionEvidence) return;
			if (RecoveryEvidence) RecoveryEvidence->Detach();
			for (std::size_t Index = 0; Index < RecoveryRecords.size(); ++Index)
				if (RecoveryRecords[Index]) RecoveryRecords[Index]->Dump(*AdmissionEvidence, RunId, OverloadCases[Index]);
		}

		void Step(std::uint64_t Tick, ServerTickTiming Timing) {
			CurrentServerTickTiming = Timing;
			if (State == Stage::Complete) return;
			if (State == Stage::OverloadReady || State == Stage::OverloadOffering ||
				State == Stage::OverloadOffered || State == Stage::OverloadRecovery) {
				StepOverload(Tick);
				return;
			}
			if (State == Stage::AwaitingClockQuiescence) {
				if (HasAllPeerAcks("ScaleClockQuiescedAcks")) {
					std::cout << "[Qualification:FarmClock] event=quiesce_complete run=" << RunId
						<< " epoch=" << CalibrationEpoch << " count=" << PeerCount << " tick=" << Tick
						<< " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
							std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
					PublishClockState(true, Tick);
					// Quiescence and active RPC calibration are separately bounded stages.
					// A late final quiescence ACK must not consume the probe budget.
					CalibrationTick = Tick;
					State = Stage::Calibrating;
					return;
				}
				if (Tick - CalibrationTick >= MaximumCalibrationTicks)
					Fail("clock_quiescence_did_not_converge", Tick);
				return;
			}
			if (State == Stage::Calibrating) {
				if (HasAllPeerAcks("ScaleClockAcks")) {
					std::cout << "[Qualification:FarmClock] event=calibration_complete run=" << RunId
						<< " epoch=" << CalibrationEpoch << " count=" << PeerCount << " tick=" << Tick << '\n';
					PublishClockState(false, Tick);
					BeginPhase(NextPhase, Tick);
					return;
				}
				if (Tick - CalibrationTick >= MaximumCalibrationTicks)
					Fail("clock_calibration_did_not_converge", Tick);
				return;
			}
			if (State == Stage::Concluding) {
				const auto Metrics = Session.GetMetrics();
				if (Metrics.ReadyPeers != PeerCount || Metrics.ReliableAdmission.TerminalReleasedBytes != 0)
					Fail("client_or_grant_left_before_completion", Tick);
				// Keep the clients connected until every accepted structural byte
				// retires; their completion observation alone is not a drain proof.
				if (Tick - ConclusionTick >= CompletionPropagationTicks &&
					detail::HasPhysicalScaleTerminalConvergence(Metrics)) {
					State = Stage::Complete;
					const auto Provider = Runtime.Content->GetMetrics();
					std::cout << "[Qualification:Scale] event=result run=" << RunId
						<< " status=PASS phases=5 peers=32 active_characters=8 content_objects=512"
						<< " native_observed=" << Metrics.NativeQueuedReliablePeersObserved
						<< " feedback_observed=" << Metrics.StructuralFeedbackPeersObserved
						<< " provider_acquisitions=" << Provider.Acquisitions
						<< " provider_admissions=" << Provider.Admissions
						<< " provider_evictions=" << Provider.Evictions << " tick=" << Tick << '\n';
					return;
				}
				if (Tick - ConclusionTick >= MaximumPhaseTicks)
					Fail("completion_did_not_converge", Tick);
				return;
			}
			if (FirstTick == 0) FirstTick = Tick;
			if (State == Stage::WaitingForPeers) {
				if (Session.GetMetrics().ReadyPeers == PeerCount && Runtime.Content->IsManifestAvailable()) {
					Initialize(Tick);
					return;
				}
				if (Tick - FirstTick >= MaximumSetupTicks) Fail("clients_or_manifest_not_ready", Tick);
				return;
			}
			if (Session.GetMetrics().ReadyPeers != PeerCount) Fail("client_left_during_scale_workload", Tick);
			if (State == Stage::Warming) {
				if (Session.GetMetrics().MaterializationBacklog == 0)
					++ConvergedWarmupTicks;
				else ConvergedWarmupTicks = 0;
				if (Tick - SetupTick >= WarmupTicks && ConvergedWarmupTicks >= 2) {
					BeginCalibration(Phase::Baseline, Tick);
					return;
				}
				if (Tick - SetupTick >= MaximumSetupTicks) Fail("initial_materialization_did_not_converge", Tick);
				return;
			}
			if (State != Stage::Measuring) Fail("invalid_controller_state", Tick);
			const auto PhaseName = Name(CurrentPhase);
			const bool ProducerDone = Runtime.CharacterControl->GetAttributeValue("ScaleRemoteDone",
				ScriptSecurityContext::CoreTrusted()) == std::optional<WireValue>(WireValue(std::string(PhaseName)));
			const bool ProducerFailed = Runtime.CharacterControl->GetAttributeValue("ScaleProducerFailed",
				ScriptSecurityContext::CoreTrusted()) == std::optional<WireValue>(WireValue(std::string(PhaseName)));
			if (ProducerFailed) Fail("producer_phase_metrics_failed", Tick);
			const bool AllPeersAcknowledged = HasAllPeerAcks("ScalePhaseAcks");
			const bool AllContentObserved = HasAllPeerAcks("ScaleContentObservedAcks");
			if (AllPeersAcknowledged && !PhaseAcksObserved) {
				PhaseAcksObserved = true;
				std::cout << "[Qualification:Scale] event=phase_acks run=" << RunId << " phase=" << PhaseName
					<< " count=" << PeerCount << " tick=" << Tick << '\n';
			}
			if (AllContentObserved && !ContentAcksObserved) {
				ContentAcksObserved = true;
				std::cout << "[Qualification:Scale] event=content_acks run=" << RunId << " phase=" << PhaseName
					<< " count=" << PeerCount << " tick=" << Tick << '\n';
			}
			if (ProducerDone && !ProducerAckObserved) {
				ProducerAckObserved = true;
				std::cout << "[Qualification:Scale] event=producer_ack run=" << RunId << " phase=" << PhaseName
					<< " tick=" << Tick << '\n';
			}
			const bool WorldConverged = IsWorldConverged(CurrentPhase);
			if (!PhaseStopRequested && Tick - PhaseTick >= MinimumPhaseTicks &&
				std::chrono::steady_clock::now() - PhaseStarted > MinimumPhaseWall &&
				AllPeersAcknowledged && AllContentObserved && WorldConverged) {
				if (Runtime.DataModel->ApplyAttributeMutation("ScalePhaseStopRequested",
					WireValue(std::string(PhaseName)), ScriptSecurityContext::CoreTrusted()) !=
					MutationStatus::Success) Fail("phase_stop_publication_rejected", Tick);
				PhaseStopRequested = true;
				std::cout << "[Qualification:Scale] event=phase_stop_requested run=" << RunId
					<< " phase=" << PhaseName << " tick=" << Tick
					<< " elapsed_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - PhaseStarted).count() << '\n';
			}
			if (PhaseStopRequested && ProducerDone && AllPeersAcknowledged && AllContentObserved && WorldConverged) {
				EndPhase(Tick);
				return;
			}
			if (Tick - PhaseTick >= MaximumPhaseTicks) {
				std::cout << "[Qualification:Scale] event=timeout run=" << RunId << " phase=" << PhaseName
					<< " producer_done=" << ProducerDone << " phase_acks=" << AllPeersAcknowledged
					<< " content_acks=" << AllContentObserved
					<< " world_converged=" << WorldConverged
					<< " materialization_backlog=" << Session.GetMetrics().MaterializationBacklog << '\n';
				Fail("phase_did_not_converge", Tick);
			}
		}

		[[nodiscard]] bool IsComplete() const { return State == Stage::Complete; }

	  private:
		// Retain the simulator's tick bounds and enforce the accepted workload's
		// separate, strictly greater than 13-second phase-boundary spacing.
		static constexpr std::uint64_t WarmupTicks = 120;
		static constexpr std::uint64_t MinimumPhaseTicks = 240;
		static constexpr auto MinimumPhaseWall = std::chrono::seconds(13);
		static constexpr std::uint64_t MaximumPhaseTicks = 1'200;
		static constexpr std::uint64_t MaximumSetupTicks = 1'200;
		static constexpr std::uint64_t CompletionPropagationTicks = 120;
		static constexpr std::uint64_t MaximumCalibrationTicks = 600;
		enum class Stage : std::uint8_t { WaitingForPeers, Warming, AwaitingClockQuiescence, Calibrating, Measuring,
			OverloadReady, OverloadOffering, OverloadOffered, OverloadRecovery,
			Concluding, Complete };
		enum class Phase : std::uint8_t { Baseline, Load, Resident, Evict, Reload };

		Engine &Runtime;
		network::GameSession &Session;
		std::string RunId;
		bool RecoveryWorkload = false;
		detail::FarmServerTickEvidence *TickEvidence = nullptr;
		Stage State = Stage::WaitingForPeers;
		Phase CurrentPhase = Phase::Baseline;
		std::uint64_t FirstTick = 0;
		std::uint64_t SetupTick = 0;
		std::uint64_t PhaseTick = 0;
		std::uint64_t CalibrationTick = 0;
		int CalibrationEpoch = 0;
		Phase NextPhase = Phase::Baseline;
		std::chrono::steady_clock::time_point PhaseStarted;
		std::uint64_t ConclusionTick = 0;
		std::uint64_t ConvergedWarmupTicks = 0;
		bool PhaseAcksObserved = false;
		bool ContentAcksObserved = false;
		bool ProducerAckObserved = false;
		bool PhaseStopRequested = false;
		ObjectId FirstContentRoot;
		std::vector<std::shared_ptr<Part>> Grounds;
		std::vector<std::shared_ptr<AnimationTrack>> RootTracks;
		std::shared_ptr<Folder> OverloadRoot;
		std::array<std::shared_ptr<Part>, 32> OverloadParts{};
		std::uint64_t CessationJournalTail = 0;
		std::uint64_t CessationAcceptedBytes = 0;
		detail::FarmAdmissionEvidence *AdmissionEvidence = nullptr;
		std::optional<network::detail::FrozenCessationQuote> CessationQuote;
		std::array<std::unique_ptr<detail::FarmRecoveryEvidence>, 3> RecoveryRecords;
		detail::FarmRecoveryEvidence *RecoveryEvidence = nullptr;
		std::uint64_t RecoveryConvergedMicroseconds = 0;
		std::map<network::ConnectionId, std::uint64_t> QuotedFutureBytes;
		std::size_t AdmissionEvidenceCursor = 0;
		std::uint64_t QuotedFrameCount = 0, AuditedFrameCount = 0, AuditedAcceptedBytes = 0;
		std::uint64_t QuoteBoundMicroseconds = 0;
		bool QuoteComplete = false, QuoteSealed = false, QuoteResultWritten = false;
		std::string QuoteFailure;
		ServerTickTiming CurrentServerTickTiming;
		network::GameSessionMetrics PreviousRecoveryMetrics;
		std::uint64_t LastQuoteAdvanceMicroseconds = 0;
		std::uint64_t LastQuoteBuildMicroseconds = 0;
		std::uint64_t LastQuoteEncodeMicroseconds = 0;
		std::uint64_t LastQuoteEncodeRetries = 0;
		bool LastQuoteAdvanceAttempted = false;
		bool LastQuoteAdvanceProducedFrame = false;
		bool RecoverySnapshotWritten = false;
		std::size_t OverloadRetainedHighWater = 0;
		std::int64_t OverloadMinimumRetentionMargin = std::numeric_limits<std::int64_t>::max();
		std::uint64_t OverloadRetentionMutationSamples = 0;
		std::size_t OverloadCase = 0;
		std::uint32_t OverloadOpportunities = 0;
		std::chrono::steady_clock::time_point OverloadStageStarted;
		std::chrono::steady_clock::time_point LastOverloadOpportunity;
		std::chrono::steady_clock::time_point OverloadCeased;
		static constexpr std::array<std::string_view, 3> OverloadCases{"gameplay", "structural", "mixed"};
		static constexpr std::uint32_t OverloadOpportunityCount = 480;
		static constexpr std::size_t OverloadNameBytes = 24 * 1024;
		// Keep the fixed 20-second service observation separate from the
		// workload-derived structural convergence deadline. This snapshot is a
		// service observation, not the structural terminal deadline.
		static constexpr auto StrictSnapshotTarget = std::chrono::microseconds(20'400'000);
		static constexpr std::uint64_t MaximumPeerRetainedCompleteBytes = 40ULL * 1024 * 1024 * 1024;
		static constexpr std::uint64_t MaximumPoolRetainedCompleteBytes = 768ULL * 1024 * 1024 * 1024;

		[[nodiscard]] static std::string_view Name(Phase Value) {
			switch (Value) {
			case Phase::Baseline: return "baseline";
			case Phase::Load: return "load";
			case Phase::Resident: return "resident";
			case Phase::Evict: return "evict";
			case Phase::Reload: return "reload";
			}
			return "invalid";
		}

		[[noreturn]] void Fail(std::string_view Reason, std::uint64_t Tick) const {
			std::cout << "[Qualification:Scale] event=result run=" << RunId << " status=FAIL reason=" << Reason
				<< " tick=" << Tick << '\n';
			try { DumpRecoveryEvidence(); }
			catch (const std::exception &Error) {
				std::cerr << "[Qualification:Recovery] evidence_write_failed=" << Error.what() << '\n';
			}
			throw std::runtime_error("physical scale qualification failed: " + std::string(Reason));
		}

		void StepCessationQuote() {
			LastQuoteAdvanceMicroseconds = 0;
			LastQuoteBuildMicroseconds = LastQuoteEncodeMicroseconds = LastQuoteEncodeRetries = 0;
			LastQuoteAdvanceAttempted = LastQuoteAdvanceProducedFrame = false;
			if (!CessationQuote || !AdmissionEvidence || !QuoteFailure.empty()) return;
			const auto Events = AdmissionEvidence->EventsSince(AdmissionEvidenceCursor);
			AdmissionEvidenceCursor += Events.size();
			for (const auto &Event : Events) {
				if (Event.Kind != network::detail::AdmissionEvidenceKind::GrantAccepted) continue;
				if (!CessationQuote->MaximumFrameBytes.contains(Event.Connection)) {
					QuoteFailure = "a new peer accepted structural work after cessation";
					return;
				}
				++AuditedFrameCount;
				AuditedAcceptedBytes += Event.ExactBytes;
			}
			if (!QuoteComplete) {
				// Keep detached replay off the production service path as much as
				// possible. One complete advance per server step preserves the exact
				// frozen frame sequence and W_i; it does not bound one advance's CPU.
				LastQuoteAdvanceAttempted = true;
				const auto AdvanceStarted = std::chrono::steady_clock::now();
				runtime_detail::WorkSample QuoteWork{};
				network::FrozenJournalQuoteStep Step;
				{
					runtime_detail::WorkCapture Capture(&QuoteWork);
					// Detached reference replay must never impersonate live source events.
					struct PauseObservation {
						network::detail::StructuralCausalEvidenceSink *Previous = network::detail::ActiveStructuralCausalEvidence;
						PauseObservation() { network::detail::ActiveStructuralCausalEvidence = nullptr; }
						~PauseObservation() { network::detail::ActiveStructuralCausalEvidence = Previous; }
					} Paused;
					Step = CessationQuote->Replication->AdvanceFrozenJournalQuote(
						CessationQuote->MaximumFrameBytes);
				}
				LastQuoteBuildMicroseconds = QuoteWork[static_cast<std::size_t>(
					runtime_detail::WorkPhase::IncrementalPreparation)].ExclusiveNanoseconds / 1000;
				LastQuoteEncodeMicroseconds = QuoteWork[static_cast<std::size_t>(
					runtime_detail::WorkPhase::StructuralEncode)].ExclusiveNanoseconds / 1000;
				LastQuoteEncodeRetries = QuoteWork.Counters[static_cast<std::size_t>(
					runtime_detail::WorkCounter::EncodeRetries)];
				LastQuoteAdvanceMicroseconds = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - AdvanceStarted).count());
				LastQuoteAdvanceProducedFrame = Step.Frame.has_value();
				if (!Step.Error.empty()) { QuoteFailure = Step.Error; return; }
				if (Step.Complete) QuoteComplete = true;
				else if (Step.Frame) {
					if (Step.Frame->CompleteBytes > network::MaximumReliableServiceGroupBytes ||
						QuotedFrameCount >= detail::FarmAdmissionEvidence::EventLimit() ||
						QuotedFutureBytes[Step.Frame->Connection] >
							std::numeric_limits<std::uint64_t>::max() - Step.Frame->CompleteBytes) {
						QuoteFailure = "cessation quote exceeded its bounded frame count or byte sum";
						return;
					}
					QuotedFutureBytes[Step.Frame->Connection] += Step.Frame->CompleteBytes;
					RecoveryEvidence->ReferenceFrame(*Step.Frame);
					++QuotedFrameCount;
				}
			}
			if (!RecoveryEvidence->Failure().empty()) QuoteFailure = RecoveryEvidence->Failure();
		}

		[[nodiscard]] static std::string QuoteReasonToken(std::string_view Reason) {
			std::string Result;
			for (const char Value : Reason.substr(0, 96)) {
				if ((Value >= 'a' && Value <= 'z') || (Value >= 'A' && Value <= 'Z') ||
					(Value >= '0' && Value <= '9') || Value == '_') Result.push_back(Value);
				else Result.push_back('_');
			}
			return Result.empty() ? "none" : Result;
		}

		void EmitQuoteResult(std::uint64_t Tick, std::uint64_t Total = 0) {
			if (QuoteResultWritten) return;
			QuoteResultWritten = true;
			std::cerr << "[Qualification:Recovery] event=quote_result run=" << RunId
				<< " case=" << OverloadCases[OverloadCase]
				<< " status=" << (QuoteFailure.empty() ? "PASS" : "NOT_MEASURED")
				<< " w_complete_upper_bytes=" << (QuoteFailure.empty() ? std::to_string(Total) : "NOT_MEASURED")
				<< " contract=causal_fence_v1 quoted_frames=" << QuotedFrameCount
				<< " bound_us=" << (QuoteFailure.empty() ? std::to_string(QuoteBoundMicroseconds) : "NOT_MEASURED")
				<< " reason=" << QuoteReasonToken(QuoteFailure)
				<< " tick=" << Tick << '\n';
		}

		void TrySealCessationQuote(const network::GameSessionMetrics &Metrics, std::uint64_t Tick) {
			if (!QuoteFailure.empty()) return;
			if (AdmissionEvidence->Overflowed()) { QuoteFailure = "admission evidence overflowed during quote audit"; return; }
			if (Metrics.ReliableAdmission.AcceptedBytes < CessationAcceptedBytes ||
				Metrics.ReliableAdmission.AcceptedBytes - CessationAcceptedBytes != AuditedAcceptedBytes ||
				!RecoveryEvidence || !RecoveryEvidence->Failure().empty() ||
				RecoveryEvidence->Audit().Totals().AcceptedBytes != AuditedAcceptedBytes + RecoveryEvidence->InitialDebt()) {
				QuoteFailure = "admission totals do not match causal accepted frames";
				return;
			}
			if (QuoteSealed || !QuoteComplete) return;
			std::map<network::ConnectionId, std::uint64_t> WorkByPeer;
			std::uint64_t Total = 0;
			for (const auto &[Connection, Debt] : CessationQuote->AcceptedUnretiredCompleteBytes) {
				const auto Future = QuotedFutureBytes[Connection];
				if (Debt > network::MaximumReliableServiceGroupBytes ||
					Future > MaximumPeerRetainedCompleteBytes - Debt ||
					Total > MaximumPoolRetainedCompleteBytes - Debt - Future) {
					QuoteFailure = "cessation quote exceeds canonical retained work bounds";
					return;
				}
				WorkByPeer.emplace(Connection, Debt + Future);
				Total += Debt + Future;
			}
			std::vector<std::uint64_t> WorkValues;
			WorkValues.reserve(WorkByPeer.size());
			for (const auto &[Connection, Work] : WorkByPeer) WorkValues.push_back(Work);
			const auto Bound = detail::RecoveryConvergenceBoundMicroseconds(WorkValues);
			if (!Bound) { QuoteFailure = "cessation quote recovery bound is invalid"; return; }
			QuoteBoundMicroseconds = *Bound;
			for (const auto &[Connection, Work] : WorkByPeer) {
			const auto Debt = CessationQuote->AcceptedUnretiredCompleteBytes.at(Connection);
			std::cerr << "[Qualification:Recovery] event=quote_peer run=" << RunId
				<< " case=" << OverloadCases[OverloadCase]
				<< " connection_slot=" << Connection.Slot
				<< " connection_generation=" << Connection.Generation
				<< " accepted_unretired_complete_bytes=" << Debt
				<< " future_complete_bytes=" << QuotedFutureBytes[Connection]
				<< " w_complete_upper_bytes=" << Work << '\n';
			}
			QuoteSealed = true;
			EmitQuoteResult(Tick, Total);
		}

		void EmitCausalResult(std::uint64_t Elapsed) {
			RecoveryEvidence->ValidateSource(Session);
			for (const auto &Peer : RecoveryEvidence->Peers()) {
				const auto Snapshot = RecoveryEvidence->Audit().Snapshot(Peer.Connection);
				if (!Snapshot) { QuoteFailure = "causal peer summary missing"; return; }
				const auto &Prefix = Snapshot->Prefix;
				std::cerr << "[Qualification:Recovery] event=causal_peer run=" << RunId
					<< " case=" << OverloadCases[OverloadCase]
					<< " connection_slot=" << Peer.Connection.Slot << " connection_generation=" << Peer.Connection.Generation
					<< " journal_fence=" << Snapshot->JournalFence << " cursor=" << Snapshot->JournalCursor
					<< " cut_token=" << Snapshot->CutGrantToken << " cut_sequence=" << Snapshot->CutSequence
					<< " unresolved=" << Snapshot->UnresolvedBaselineTokens
					<< " planning_unresolved=" << Snapshot->HasUnresolvedPlanning
					<< " accepted=" << Prefix.AcceptedBytes << " first_sent=" << Prefix.FirstSentBytes
					<< " acked=" << Prefix.AckedBytes << " retired=" << Prefix.RetiredBytes
					<< " converged=" << Snapshot->Converged << '\n';
			}
			const auto &Totals = RecoveryEvidence->Audit().Totals();
			std::cerr << "[Qualification:Recovery] event=causal_result run=" << RunId
				<< " case=" << OverloadCases[OverloadCase] << " contract=causal_fence_v1"
				<< " status=" << (RecoveryEvidence->Converged() ? "PASS" : "FAIL")
				<< " events=" << RecoveryEvidence->EventCount()
				<< " accepted=" << Totals.AcceptedBytes << " first_sent=" << Totals.FirstSentBytes
				<< " acked=" << Totals.AckedBytes << " retired=" << Totals.RetiredBytes
				<< " prefix_converged_us=" << RecoveryConvergedMicroseconds << " elapsed_us=" << Elapsed << '\n';
		}

		void Initialize(std::uint64_t Tick) {
			const auto *Manifest = Runtime.Content->GetManifest();
			if (!Manifest || Manifest->Entries.size() != 1 || Manifest->Entries.front().Key != ContentKey ||
				Manifest->Entries.front().ObjectCount != ContentObjects ||
				Manifest->Entries.front().UncompressedBytes != ContentPayloadBytes)
				Fail("canonical_content_manifest_mismatch", Tick);
			auto Identities = Session.GetPeerIdentities();
			if (Identities.size() != PeerCount) Fail("peer_identity_count_mismatch", Tick);
			std::array<std::optional<network::GameSessionPeerIdentity>, PeerCount> BySlot;
			const auto Prefix = Identities.front().Nonce >> 32;
			if (Prefix == 0) Fail("missing_run_scoped_nonce_prefix", Tick);
			for (const auto &Identity : Identities) {
				const auto SlotPlusOne = static_cast<std::uint32_t>(Identity.Nonce);
				if (!Identity.Ready || Identity.Nonce >> 32 != Prefix || SlotPlusOne < 1 || SlotPlusOne > PeerCount ||
					BySlot[SlotPlusOne - 1].has_value()) Fail("invalid_or_duplicate_peer_nonce", Tick);
				BySlot[SlotPlusOne - 1] = Identity;
			}
			if (!BySlot[0] || BySlot[0]->PlayerId != 1) Fail("producer_slot_identity_mismatch", Tick);
			const auto ProducerConnection = BySlot[0]->Connection;
			if (!ProducerConnection.IsValid() ||
				ProducerConnection.Slot > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
				ProducerConnection.Generation > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
				Fail("producer_connection_identity_out_of_range", Tick);
			if (Runtime.DataModel->ApplyAttributeMutation("ScaleProducerConnectionSlot",
				WireValue(static_cast<int>(ProducerConnection.Slot)), ScriptSecurityContext::CoreTrusted()) !=
				MutationStatus::Success ||
				Runtime.DataModel->ApplyAttributeMutation("ScaleProducerConnectionGeneration",
					WireValue(static_cast<int>(ProducerConnection.Generation)), ScriptSecurityContext::CoreTrusted()) !=
					MutationStatus::Success)
				Fail("producer_connection_identity_publication_rejected", Tick);
			for (std::size_t Group = 0; Group < PeerCount / NeighborhoodSize; ++Group) {
				auto Ground = std::make_shared<Part>();
				Ground->SetName("ScaleResidentGround");
				Ground->SetAnchored(true);
				Ground->SetSize({1024.0f, 1.0f, 1024.0f});
				Ground->SetPosition({static_cast<float>(Group + 1) * 2048.0f, 0.0f, 0.0f});
				Ground->SetParent(Runtime.Workspace);
				Grounds.push_back(std::move(Ground));
			}
			std::size_t Active = 0;
			for (std::size_t Slot = 0; Slot < PeerCount; ++Slot) {
				if (!BySlot[Slot]) Fail("missing_peer_slot", Tick);
				const auto &Identity = *BySlot[Slot];
				auto PlayerValue = Session.GetAcceptedPlayer(Identity.Connection);
				if (!PlayerValue || !PlayerValue->GetCharacter()) Fail("ready_peer_has_no_character", Tick);
				auto Character = std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
				if (!Character) Fail("peer_character_type_mismatch", Tick);
				const auto Neighborhood = Slot / NeighborhoodSize + 1;
				const auto Local = Slot % NeighborhoodSize;
				const glm::vec3 Position{
					static_cast<float>(Neighborhood) * 2048.0f + static_cast<float>(Local % 5) * 3.0f,
					6.0f, static_cast<float>(Local / 5) * 3.0f
				};
				Character->SetPosition(Position);
				const std::array Focus{Position, glm::vec3(0.0f, 6.0f, 0.0f)};
				if (!Session.SetTrustedReplicationFocus(Identity.Connection, Focus))
					Fail("trusted_relevance_focus_rejected", Tick);
				if (Slot % (NeighborhoodSize / 2) != 0) {
					PlayerValue->RemoveCharacter();
					continue;
				}
				++Active;
				auto Rig = std::make_shared<MeshPart>();
				Rig->SetName("ScaleAnimationRig");
				Rig->SetMesh("asset://d549080bd1e64aaee8041f4ece3e9f75");
				Rig->SetAnchored(true);
				Rig->SetCanCollide(false);
				Rig->SetCanTouch(false);
				Rig->SetCFrame(Character->GetCFrame());
				Rig->SetParent(Character);
				auto AnimatorValue = std::make_shared<Animator>();
				AnimatorValue->SetParent(Rig);
				auto Track = AnimatorValue->CreateTrack("asset://d9d9e9649adbad59588d137c2a642e1d");
				if (!Track) Fail("root_motion_track_creation_failed", Tick);
				Track->SetRootMotionEnabled(true);
				Track->SetLooped(true);
				Track->Play();
				RootTracks.push_back(std::move(Track));
				std::cout << "[Qualification:Scale] event=tracked_root run=" << RunId
					<< " index=" << Active - 1 << " client_index=" << Slot
					<< " peer_slot=" << Identity.Connection.Slot
					<< " peer_generation=" << Identity.Connection.Generation
					<< " neighborhood=" << Neighborhood
					<< " recipient_first=" << (Neighborhood - 1) * NeighborhoodSize
					<< " recipient_last=" << Neighborhood * NeighborhoodSize - 1
					<< " object_slot=" << Character->GetObjectId().Slot
					<< " object_generation=" << Character->GetObjectId().Generation
					<< " tick=" << Tick << '\n';
			}
			if (Active != ActiveCharacters || RootTracks.size() != ActiveCharacters)
				Fail("active_character_count_mismatch", Tick);
			SetupTick = Tick;
			State = Stage::Warming;
			std::cout << "[Qualification:Scale] event=setup run=" << RunId << " peers=" << PeerCount
				<< " active_characters=" << Active << " spectators=" << PeerCount - Active
				<< " neighborhoods=" << PeerCount / NeighborhoodSize << " tracks=" << RootTracks.size()
				<< " content_objects=" << Manifest->Entries.front().ObjectCount
				<< " content_bytes=" << Manifest->Entries.front().UncompressedBytes << " tick=" << Tick << '\n';
		}

		[[nodiscard]] std::shared_ptr<Instance> GetContentRoot() const {
			return Runtime.Workspace->FindFirstChild(std::string(ContentRootName), false);
		}

		[[nodiscard]] bool IsWorldConverged(Phase Value) const {
			if (Session.GetMetrics().MaterializationBacklog != 0) return false;
			const auto StateValue = Runtime.Content->GetState(ContentKey);
			if (!StateValue) return false;
			const auto Root = GetContentRoot();
			if (Value == Phase::Baseline || Value == Phase::Evict)
				return !Root && (*StateValue == ContentResidencyState::Unavailable ||
					*StateValue == ContentResidencyState::Available);
			if (*StateValue != ContentResidencyState::Resident || !Root ||
				Root->GetDescendants().size() + 1 != ContentObjects) return false;
			if (Value == Phase::Reload && Root->GetObjectId() == FirstContentRoot) return false;
			return true;
		}

		void BeginPhase(Phase Value, std::uint64_t Tick) {
			if (Value == Phase::Load || Value == Phase::Reload) {
				if (!Runtime.Content->RequestContent(ContentKey)) Fail("content_demand_rejected", Tick);
			} else if (Value == Phase::Evict) {
				if (!Runtime.Content->ReleaseContent(ContentKey)) Fail("content_release_rejected", Tick);
			}
			if (Runtime.DataModel->ApplyAttributeMutation("ScalePhaseStopRequested", WireValue(std::string("none")),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("phase_stop_reset_rejected", Tick);
			if (Runtime.DataModel->ApplyAttributeMutation("ScalePhase", WireValue(std::string(Name(Value))),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("phase_publication_rejected", Tick);
			CurrentPhase = Value;
			PhaseTick = Tick;
			PhaseStarted = std::chrono::steady_clock::now();
			if (TickEvidence) TickEvidence->PhaseStart(static_cast<std::uint8_t>(Value), Tick);
			PhaseAcksObserved = false;
			ContentAcksObserved = false;
			ProducerAckObserved = false;
			PhaseStopRequested = false;
			State = Stage::Measuring;
			std::cout << "[Qualification:Scale] event=phase_start run=" << RunId << " phase=" << Name(Value)
				<< " tick=" << Tick << " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
					PhaseStarted.time_since_epoch()).count() << '\n';
		}

		void PublishClockState(bool Active, std::uint64_t Tick) {
			if (Runtime.DataModel->ApplyAttributeMutation("ScaleClockActive", WireValue(Active),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("clock_state_publication_rejected", Tick);
		}

		void BeginCalibration(Phase Value, std::uint64_t Tick) {
			if (CalibrationEpoch >= 5) Fail("clock_epoch_limit_exceeded", Tick);
			++CalibrationEpoch;
			NextPhase = Value;
			CalibrationTick = Tick;
			// Close the previous measured callback window before any probe offer.
			if (Runtime.DataModel->ApplyAttributeMutation("ScalePhase", WireValue(std::string("calibrating")),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("clock_phase_publication_rejected", Tick);
			if (Runtime.DataModel->ApplyAttributeMutation("ScaleClockEpoch", WireValue(CalibrationEpoch),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("clock_epoch_publication_rejected", Tick);
			PublishClockState(false, Tick);
			State = Stage::AwaitingClockQuiescence;
			std::cout << "[Qualification:FarmClock] event=calibration_start run=" << RunId
				<< " epoch=" << CalibrationEpoch << " next_phase=" << Name(Value) << " tick=" << Tick
				<< " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
		}

		void PublishOverloadAttribute(std::string_view Key, WireValue Value, std::uint64_t Tick) {
			if (Runtime.DataModel->ApplyAttributeMutation(std::string(Key), std::move(Value),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("overload_control_publication_rejected", Tick);
		}

		[[nodiscard]] bool HasAllPeerAcks(std::string_view Key) const {
			const auto Value = Runtime.CharacterControl->GetAttributeValue(std::string(Key),
				ScriptSecurityContext::CoreTrusted());
			return detail::DecodePhysicalScaleCounter(Value) == static_cast<std::uint32_t>(PeerCount);
		}

		struct RetentionObservation {
			std::size_t Retained = 0;
			std::uint64_t Oldest = 1;
			std::uint64_t Required = 1;
			std::int64_t Margin = 0;
		};

		[[nodiscard]] RetentionObservation ObserveRetention(std::uint64_t Tick) {
			const auto Window = ChangeJournal::Get().GetRetentionWindow(Runtime.DataModel->GetObjectId());
			auto Required = Window.NextSequence;
			for (const auto &Reader : network::detail::GameSessionTestAccess::GetJournalRequirements(Session))
				Required = std::min(Required, Reader.Cursor.NextSequence);
			const auto Margin = static_cast<std::int64_t>(Required) - static_cast<std::int64_t>(Window.OldestSequence);
			OverloadRetainedHighWater = std::max(OverloadRetainedHighWater, Window.RetainedRecords);
			OverloadMinimumRetentionMargin = std::min(OverloadMinimumRetentionMargin, Margin);
			if (Margin < 0) Fail("journal_reader_evicted_during_overload", Tick);
			return {Window.RetainedRecords, Window.OldestSequence, Required, Margin};
		}

		void BeginOverloadCase(std::uint64_t Tick) {
			if (OverloadCase >= OverloadCases.size()) Fail("overload_case_out_of_range", Tick);
			OverloadOpportunities = 0;
			OverloadRetainedHighWater = 0;
			OverloadMinimumRetentionMargin = std::numeric_limits<std::int64_t>::max();
			OverloadRetentionMutationSamples = 0;
			PublishOverloadAttribute("ScaleOverloadCase", WireValue(std::string(OverloadCases[OverloadCase])), Tick);
			OverloadStageStarted = std::chrono::steady_clock::now();
			State = Stage::OverloadReady;
			std::cout << "[Qualification:Recovery] event=case_start run=" << RunId
				<< " case=" << OverloadCases[OverloadCase] << " tick=" << Tick
				<< " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
					OverloadStageStarted.time_since_epoch()).count() << '\n';
		}

		void InitializeOverload(std::uint64_t Tick) {
			OverloadRoot = std::make_shared<Folder>();
			OverloadRoot->SetName("ScaleOverloadRegion");
			OverloadRoot->SetParent(Runtime.Workspace);
			for (std::size_t Index = 0; Index < OverloadParts.size(); ++Index) {
				auto Holder = std::make_shared<Folder>();
				Holder->SetName("part" + std::to_string(Index));
				Holder->SetParent(OverloadRoot);
				auto Value = std::make_shared<Part>();
				Value->SetName("initial");
				Value->SetAnchored(true);
				Value->SetCanCollide(false);
				Value->SetCanTouch(false);
				Value->SetPosition({0.0f, 8.0f, 0.0f});
				Value->SetParent(Holder);
				std::cout << "[Qualification:Recovery] event=object run=" << RunId
					<< " index=" << Index << " object_slot=" << Value->GetObjectId().Slot
					<< " object_generation=" << Value->GetObjectId().Generation << '\n';
				OverloadParts[Index] = std::move(Value);
			}
			PublishOverloadAttribute("ScaleOverloadEnabled", WireValue(true), Tick);
			BeginOverloadCase(Tick);
		}

		void OfferOverloadNameWork(std::uint64_t Tick) {
			if (OverloadCase == 0) return;
			const auto First = ((OverloadOpportunities - 1) % 2) * 16;
			for (std::size_t Index = First; Index < First + 16; ++Index) {
				const auto Letter = static_cast<char>('a' + (OverloadCase * 7 + OverloadOpportunities + Index) % 26);
				const auto TailBefore = ChangeJournal::Get().CreateCursor(
					Runtime.DataModel->GetObjectId()).NextSequence;
				OverloadParts[Index]->SetName(std::string(OverloadNameBytes, Letter));
				if (ChangeJournal::Get().CreateCursor(Runtime.DataModel->GetObjectId()).NextSequence <= TailBefore)
					Fail("overload_name_mutation_not_committed", Tick);
				// Sample immediately after each committed workload mutation, not
				// merely after the sixteen-mutation opportunity has completed.
				(void)ObserveRetention(Tick);
				++OverloadRetentionMutationSamples;
			}
			std::cout << "[Qualification:Recovery] event=structural_offer run=" << RunId
				<< " case=" << OverloadCases[OverloadCase] << " opportunity=" << OverloadOpportunities
				<< " mutations=16 name_bytes=" << OverloadNameBytes << " tick=" << Tick
				<< " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
			const auto Retention = ObserveRetention(Tick);
			std::cout << "[Qualification:Recovery] event=retention run=" << RunId
				<< " case=" << OverloadCases[OverloadCase] << " opportunity=" << OverloadOpportunities
				<< " retained=" << Retention.Retained << " oldest=" << Retention.Oldest
				<< " required=" << Retention.Required << " margin=" << Retention.Margin
				<< " mutation_samples=" << OverloadRetentionMutationSamples << '\n';
		}

		void StepOverload(std::uint64_t Tick) {
			const auto Now = std::chrono::steady_clock::now();
			const auto CaseName = OverloadCases[OverloadCase];
			if (Session.GetMetrics().ReadyPeers != PeerCount) Fail("client_left_during_overload", Tick);
			if (State == Stage::OverloadReady) {
				if (HasAllPeerAcks("ScaleOverloadReadyAcks") &&
					Session.GetMetrics().MaterializationBacklog == 0) {
					State = Stage::OverloadOffering;
					OverloadStageStarted = Now;
					LastOverloadOpportunity = Now - std::chrono::microseconds(16'667);
					std::cout << "[Qualification:Recovery] event=all_ready run=" << RunId
						<< " case=" << CaseName << " tick=" << Tick << '\n';
				} else if (Now - OverloadStageStarted > std::chrono::seconds(30))
					Fail("overload_clients_not_ready", Tick);
				return;
			}
			if (State == Stage::OverloadOffering) {
				if (Now - LastOverloadOpportunity < std::chrono::microseconds(16'667)) return;
				LastOverloadOpportunity = Now;
				++OverloadOpportunities;
				OfferOverloadNameWork(Tick);
				if (OverloadCase == 0) (void)ObserveRetention(Tick);
				if (OverloadOpportunities == OverloadOpportunityCount) {
					State = Stage::OverloadOffered;
					std::cout << "[Qualification:Recovery] event=all_opportunities run=" << RunId
						<< " case=" << CaseName << " opportunities=" << OverloadOpportunities
						<< " elapsed_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
							Now - OverloadStageStarted).count() << " tick=" << Tick << '\n';
					OverloadStageStarted = Now;
				}
			return;
			}
			if (State == Stage::OverloadOffered) {
				if (HasAllPeerAcks("ScaleOverloadOfferedAcks")) {
					PublishOverloadAttribute("ScaleOverloadCase", WireValue("recover_" + std::string(CaseName)), Tick);
					OverloadCeased = std::chrono::steady_clock::now();
					CessationJournalTail = ChangeJournal::Get().CreateCursor(
						Runtime.DataModel->GetObjectId()).NextSequence;
					CessationQuote.reset();
					if (RecoveryEvidence) RecoveryEvidence->Detach();
					RecoveryEvidence = nullptr;
					RecoveryConvergedMicroseconds = 0;
					QuotedFutureBytes.clear();
					QuotedFrameCount = AuditedFrameCount = AuditedAcceptedBytes = 0;
					QuoteBoundMicroseconds = 0;
					QuoteComplete = QuoteSealed = QuoteResultWritten = false;
					QuoteFailure.clear();
					std::uint64_t QuoteCaptureMicroseconds = 0;
					if (!AdmissionEvidence) QuoteFailure = "admission evidence is unavailable";
					else {
						AdmissionEvidenceCursor = static_cast<std::size_t>(AdmissionEvidence->Count());
						const auto CaptureStarted = std::chrono::steady_clock::now();
						CessationQuote = network::detail::GameSessionTestAccess::CaptureFrozenCessationQuote(
							Session, QuoteFailure);
						QuoteCaptureMicroseconds = static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::microseconds>(
								std::chrono::steady_clock::now() - CaptureStarted).count());
						if (!CessationQuote && QuoteFailure.empty()) QuoteFailure = "cessation quote capture failed";
						if (CessationQuote) {
							auto Snapshot = network::detail::GameSessionTestAccess::CaptureStructuralCausalSnapshot(Session, QuoteFailure);
							if (Snapshot) {
								RecoveryRecords[OverloadCase] = std::make_unique<detail::FarmRecoveryEvidence>(std::move(*Snapshot));
								RecoveryEvidence = RecoveryRecords[OverloadCase].get();
								QuoteFailure = RecoveryEvidence->Failure();
							}
						}
					}
					std::cerr << "[Qualification:Recovery] event=quote_capture run=" << RunId
						<< " case=" << CaseName << " status=" << (CessationQuote ? "READY" : "NOT_MEASURED")
						<< " capture_us=" << QuoteCaptureMicroseconds
						<< " reason=" << QuoteReasonToken(QuoteFailure)
						<< " tick=" << Tick << '\n';
					RecoverySnapshotWritten = false;
					State = Stage::OverloadRecovery;
					const auto Metrics = Session.GetMetrics();
					PreviousRecoveryMetrics = Metrics;
					CessationAcceptedBytes = Metrics.ReliableAdmission.AcceptedBytes;
					std::cout << "[Qualification:Recovery] event=cessation run=" << RunId
						<< " case=" << CaseName << " tick=" << Tick
						<< " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
							OverloadCeased.time_since_epoch()).count()
						<< " journal_tail=" << CessationJournalTail
						<< " raw_name_bytes=" << (OverloadCase == 0 ? 0 :
								OverloadOpportunityCount * 16 * OverloadNameBytes)
						<< " retained_high=" << OverloadRetainedHighWater
						<< " minimum_retention_margin=" << OverloadMinimumRetentionMargin
						<< " retention_mutation_samples=" << OverloadRetentionMutationSamples
						<< " journal_backlog=" << Metrics.JournalBacklogRecords
						<< " outstanding=" << Metrics.ReliableAdmission.OutstandingBytes
						<< " scheduler_queued=" << Metrics.SchedulerQueuedReliableBytes
						<< " native_queued=" << Metrics.NativeQueuedReliableBytes
						<< " native_observed=" << Metrics.NativeQueuedReliablePeersObserved << '\n';
					// Luau print uses this same redirected stderr stream. These barrier
					// markers order remote ACKs without comparing endpoint clocks.
					std::cerr << "[Qualification:Recovery] event=cessation_barrier run=" << RunId
						<< " case=" << CaseName << " journal_tail=" << CessationJournalTail << '\n';
				} else if (Now - OverloadStageStarted > std::chrono::seconds(30))
					Fail("overload_client_offers_incomplete", Tick);
				return;
			}
			if (State == Stage::OverloadRecovery) {
				const auto QuoteStarted = std::chrono::steady_clock::now();
				StepCessationQuote();
				const auto QuoteMicroseconds = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - QuoteStarted).count());
				const auto Metrics = Session.GetMetrics();
				const auto RelevanceMicroseconds =
					(Metrics.RelevanceCpuNanoseconds - PreviousRecoveryMetrics.RelevanceCpuNanoseconds) / 1000;
				const auto PlanningMicroseconds =
					(Metrics.PlanningCpuNanoseconds - PreviousRecoveryMetrics.PlanningCpuNanoseconds) / 1000;
				const auto MaterializationMicroseconds =
					(Metrics.MaterializationCpuNanoseconds - PreviousRecoveryMetrics.MaterializationCpuNanoseconds) / 1000;
				const auto SelectionMicroseconds =
					(Metrics.StructuralSelectionCpuNanoseconds - PreviousRecoveryMetrics.StructuralSelectionCpuNanoseconds) / 1000;
				const auto EncodedBytes = Metrics.StructuralBytesEncoded - PreviousRecoveryMetrics.StructuralBytesEncoded;
				const auto JournalRecords = Metrics.JournalRecordsExamined - PreviousRecoveryMetrics.JournalRecordsExamined;
				const auto PlanningWork = Metrics.PlanningWork - PreviousRecoveryMetrics.PlanningWork;
				PreviousRecoveryMetrics = Metrics;
				const auto Remote = network::detail::GameSessionTestAccess::GetRemoteMetrics(Session);
				TrySealCessationQuote(Metrics, Tick);
				if (!QuoteFailure.empty()) {
					if (QuoteResultWritten)
						std::cerr << "[Qualification:Recovery] event=quote_revoked run=" << RunId
							<< " case=" << CaseName << " reason=" << QuoteReasonToken(QuoteFailure)
							<< " tick=" << Tick << '\n';
					EmitQuoteResult(Tick);
					Fail("recovery_quote_invalid", Tick);
				}
				const auto Retention = ObserveRetention(Tick);
				// Quote replay can take longer than a server step. Timestamp the
				// observed metrics after replay, never with the pre-replay Now.
				const auto ObservedAt = std::chrono::steady_clock::now();
				const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					ObservedAt - OverloadCeased).count();
				if (!RecoveryConvergedMicroseconds && RecoveryEvidence->Converged())
					RecoveryConvergedMicroseconds = static_cast<std::uint64_t>(Elapsed);
				std::uint64_t QuoteLagRecords = 0;
				if (CessationQuote)
					for (const auto &[Connection, Limit] : CessationQuote->MaximumFrameBytes) {
						(void)Limit;
						QuoteLagRecords += CessationQuote->Replication->GetJournalLag(Connection);
					}
				// The binary tick record retains every server step. Keep textual
				// recovery samples bounded when a large exact quote spans many ticks,
				// while always recording the two acceptance observation points.
				const bool SnapshotDue = !RecoverySnapshotWritten &&
					ObservedAt - OverloadCeased >= StrictSnapshotTarget;
				const bool DeadlineDue = (RecoverySnapshotWritten || SnapshotDue) && QuoteSealed &&
					ObservedAt - OverloadCeased >= std::chrono::microseconds(QuoteBoundMicroseconds);
				if (Tick % 16 == 0 || SnapshotDue || DeadlineDue)
					std::cerr << "[Qualification:Recovery] event=sample run=" << RunId
					<< " case=" << CaseName << " elapsed_us=" << Elapsed
					<< " poll_us=" << CurrentServerTickTiming.PollMicroseconds
					<< " engine_us=" << CurrentServerTickTiming.EngineMicroseconds
					<< " session_us=" << CurrentServerTickTiming.SessionMicroseconds
					<< " session_incremental_us=" << CurrentServerTickTiming.SessionIncrementalMicroseconds
					<< " session_build_us=" << CurrentServerTickTiming.SessionBuildMicroseconds
					<< " session_encode_us=" << CurrentServerTickTiming.SessionEncodeMicroseconds
					<< " session_encode_retries=" << CurrentServerTickTiming.SessionEncodeRetries
					<< " relevance_us=" << RelevanceMicroseconds
					<< " planning_us=" << PlanningMicroseconds
					<< " materialization_us=" << MaterializationMicroseconds
					<< " selection_us=" << SelectionMicroseconds
					<< " encoded_bytes=" << EncodedBytes
					<< " journal_records=" << JournalRecords
					<< " planning_work=" << PlanningWork
					<< " prequalification_us=" << CurrentServerTickTiming.PreQualificationMicroseconds
					<< " quote_us=" << QuoteMicroseconds
					<< " quote_advance_us=" << LastQuoteAdvanceMicroseconds
					<< " quote_build_us=" << LastQuoteBuildMicroseconds
					<< " quote_encode_us=" << LastQuoteEncodeMicroseconds
					<< " quote_encode_retries=" << LastQuoteEncodeRetries
					<< " quote_advance_attempted=" << LastQuoteAdvanceAttempted
					<< " quote_advance_frame=" << LastQuoteAdvanceProducedFrame
					<< " quote_complete=" << QuoteComplete << " quote_sealed=" << QuoteSealed
					<< " quote_frames=" << QuotedFrameCount << " quote_audited=" << AuditedFrameCount
					<< " quote_lag_records=" << QuoteLagRecords
					<< " outstanding=" << Metrics.ReliableAdmission.OutstandingBytes
					<< " active_grants=" << Metrics.ReliableAdmission.ActiveDrainGrants
					<< " scheduler_queued=" << Metrics.SchedulerQueuedReliableBytes
					<< " native_queued=" << Metrics.NativeQueuedReliableBytes
					<< " remote_dispatch_messages=" << Remote.QueuedDispatchMessages
					<< " remote_dispatch_bytes=" << Remote.QueuedDispatchBytes
					<< " remote_deferred_messages=" << Remote.DeferredReliableMessages
					<< " remote_deferred_bytes=" << Remote.DeferredReliableBytes
					<< " remote_inflight_requests=" << Remote.InFlightRequests
					<< " native_observed=" << Metrics.NativeQueuedReliablePeersObserved
					<< " feedback_observed=" << Metrics.StructuralFeedbackPeersObserved
					<< " accepted=" << Metrics.StructuralAcceptedFeedbackBytes
					<< " first_sent=" << Metrics.StructuralFirstSentFeedbackBytes
					<< " acked=" << Metrics.StructuralAckedFeedbackBytes
					<< " retired=" << Metrics.ReliableAdmission.VerifiedAttributedRetirement
					<< " terminal_release=" << Metrics.ReliableAdmission.TerminalReleasedBytes
					<< " journal_backlog=" << Metrics.JournalBacklogRecords
					<< " materialization_backlog=" << Metrics.MaterializationBacklog
					<< " pending_enters=" << Metrics.StructuralPendingEnters
					<< " pending_leaves=" << Metrics.StructuralPendingLeaves
					<< " current_tail=" << ChangeJournal::Get().CreateCursor(
						Runtime.DataModel->GetObjectId()).NextSequence
					<< " retained=" << Retention.Retained << " oldest=" << Retention.Oldest
					<< " required=" << Retention.Required << " margin=" << Retention.Margin
					<< " retained_high=" << OverloadRetainedHighWater
					<< " minimum_retention_margin=" << OverloadMinimumRetentionMargin
					<< " journal_lag_high=" << Metrics.StructuralMaximumJournalLagRecords
					<< " journal_failures=" << Metrics.StructuralJournalLagFailures
					<< " tick=" << Tick << '\n';
				if (SnapshotDue) {
					RecoveryEvidence->ValidateSource(Session);
					RecoverySnapshotWritten = true;
					for (const auto &Reader : network::detail::GameSessionTestAccess::GetJournalRequirements(Session))
						std::cerr << "[Qualification:Recovery] event=reader run=" << RunId
							<< " case=" << CaseName << " catalog=" << Reader.Catalog
							<< " connection_slot=" << Reader.Connection.Slot
							<< " connection_generation=" << Reader.Connection.Generation
							<< " next_sequence=" << Reader.Cursor.NextSequence
							<< " prepared=" << Reader.PreparedCommit
							<< " pending_relevance=" << Reader.PendingRelevance << '\n';
					std::cerr << "[Qualification:Recovery] event=strict_snapshot run=" << RunId
						<< " case=" << CaseName << " elapsed_us=" << Elapsed
						<< " cessation_tail=" << CessationJournalTail
						<< " retained_work_bytes=NOT_MEASURED tick=" << Tick << '\n';
				}
				if (DeadlineDue) {
					EmitCausalResult(Elapsed);
					RecoveryEvidence->Detach();
					if (RecoveryEvidence->Converged())
						for (const auto &Reader : network::detail::GameSessionTestAccess::GetJournalRequirements(Session))
							std::cerr << "[Qualification:Recovery] event=terminal_reader run=" << RunId
								<< " case=" << CaseName << " catalog=" << Reader.Catalog
								<< " connection_slot=" << Reader.Connection.Slot
								<< " connection_generation=" << Reader.Connection.Generation
								<< " next_sequence=" << Reader.Cursor.NextSequence
								<< " prepared=" << Reader.PreparedCommit
								<< " pending_relevance=" << Reader.PendingRelevance << '\n';
					std::cerr << "[Qualification:Recovery] event=strict_deadline_barrier run=" << RunId
						<< " case=" << CaseName << " elapsed_us=" << Elapsed
						<< " bound_us=" << QuoteBoundMicroseconds << '\n';
					if (!RecoveryEvidence->Converged() || !RecoveryConvergedMicroseconds ||
						RecoveryConvergedMicroseconds > QuoteBoundMicroseconds)
						Fail("recovery_work_did_not_converge_within_bound", Tick);
					if (++OverloadCase < OverloadCases.size()) BeginOverloadCase(Tick);
					else PublishCompletion(Tick);
				}
			}
		}

		void PublishCompletion(std::uint64_t Tick) {
			// The measured phases, including continuing-motion recovery, have
			// finished. Stop the workload before requiring whole-run quiescence.
			for (const auto &Track : RootTracks) Track->Stop();
			if (Runtime.DataModel->ApplyAttributeMutation("ScalePhase", WireValue(std::string("complete")),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("completion_publication_rejected", Tick);
			ConclusionTick = Tick;
			State = Stage::Concluding;
			std::cout << "[Qualification:Scale] event=completion_published run=" << RunId
				<< " phase=complete propagation_ticks=" << CompletionPropagationTicks << " tick=" << Tick << '\n';
		}

		void EndPhase(std::uint64_t Tick) {
			const auto Ended = std::chrono::steady_clock::now();
			if (TickEvidence) TickEvidence->PhaseEnd(static_cast<std::uint8_t>(CurrentPhase), Tick);
			const auto Metrics = Session.GetMetrics();
			const auto Provider = Runtime.Content->GetMetrics();
			const auto Root = GetContentRoot();
			std::cout << "[Qualification:Scale] event=phase_end run=" << RunId << " phase=" << Name(CurrentPhase)
				<< " ticks=" << Tick - PhaseTick << " elapsed_us=" <<
					std::chrono::duration_cast<std::chrono::microseconds>(Ended - PhaseStarted).count()
				<< " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
					Ended.time_since_epoch()).count()
				<< " producer_done=1 phase_acks=" << PeerCount << " content_acks=" << PeerCount
				<< " observed_objects=" << (Root ? Root->GetDescendants().size() + 1 : 0)
				<< " materialization_backlog=" << Metrics.MaterializationBacklog
				<< " structural_bytes=" << Metrics.StructuralBytesEncoded
				<< " provider_requests=" << Provider.Requests << " provider_acquisitions=" << Provider.Acquisitions
				<< " provider_admissions=" << Provider.Admissions << " provider_evictions=" << Provider.Evictions
				<< " provider_failures=" << Provider.Failures << " tick=" << Tick << '\n';
			if (Provider.Failures != 0) Fail("provider_failure", Tick);
			switch (CurrentPhase) {
			case Phase::Baseline:
				BeginCalibration(Phase::Load, Tick);
				break;
			case Phase::Load:
				if (Provider.Acquisitions != 1 || Provider.Admissions != 1 || !Root)
					Fail("shared_first_admission_mismatch", Tick);
				FirstContentRoot = Root->GetObjectId();
				BeginCalibration(Phase::Resident, Tick);
				break;
			case Phase::Resident:
				BeginCalibration(Phase::Evict, Tick);
				break;
			case Phase::Evict:
				if (Provider.Evictions != 1) Fail("content_eviction_mismatch", Tick);
				BeginCalibration(Phase::Reload, Tick);
				break;
			case Phase::Reload:
				if (Provider.Acquisitions != 1 || Provider.Admissions != 2 || !Root ||
					Root->GetObjectId() == FirstContentRoot) Fail("fresh_reload_mismatch", Tick);
				if (RecoveryWorkload) InitializeOverload(Tick);
				else PublishCompletion(Tick);
				break;
			}
		}
	};
} // namespace gargantuan::host
