#pragma once

#include "gargantuan/Engine.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan::host {
	// Trusted, bounded host-only controller for the accepted 32-client recipient
	// workload. The package's server script relays the authoritative DataModel
	// ScalePhase attribute to the single producer through ScalePhaseControl and
	// records all client phase ACKs and the producer completion ACK on CharacterControl.
	// This class neither injects relevance/Desired state nor bypasses admission.
	class PhysicalScaleQualification final {
	  public:
		static constexpr std::size_t PeerCount = 32;
		static constexpr std::size_t ActiveCharacters = 8;
		static constexpr std::size_t NeighborhoodSize = 8;
		static constexpr std::size_t ContentObjects = 512;
		static constexpr std::uint64_t ContentPayloadBytes = 273'032;
		static constexpr std::string_view ContentKey = "workspace/scale";
		static constexpr std::string_view ContentRootName = "ContentScaleRegion";

		PhysicalScaleQualification(Engine &RuntimeValue, network::GameSession &SessionValue, std::string RunIdValue)
			: Runtime(RuntimeValue), Session(SessionValue), RunId(std::move(RunIdValue)) {
			if (RunId.empty() || !Runtime.Content || !Runtime.DataModel || !Runtime.CharacterControl)
				throw std::invalid_argument("physical scale qualification requires a content-backed server runtime");
		}

		void Step(std::uint64_t Tick) {
			if (State == Stage::Complete) return;
			if (State == Stage::Concluding) {
				// The final phase must reach the clients before the host closes GNS.
				// A client may finish and disconnect during this bounded interval.
				if (Tick - ConclusionTick >= CompletionPropagationTicks) {
					State = Stage::Complete;
					const auto Provider = Runtime.Content->GetMetrics();
					std::cout << "[Qualification:Scale] event=result run=" << RunId
						<< " status=PASS phases=5 peers=32 active_characters=8 content_objects=512"
						<< " provider_acquisitions=" << Provider.Acquisitions
						<< " provider_admissions=" << Provider.Admissions
						<< " provider_evictions=" << Provider.Evictions << " tick=" << Tick << '\n';
				}
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
					BeginPhase(Phase::Baseline, Tick);
					return;
				}
				if (Tick - SetupTick >= MaximumSetupTicks) Fail("initial_materialization_did_not_converge", Tick);
				return;
			}
			if (State != Stage::Measuring) Fail("invalid_controller_state", Tick);
			const auto PhaseName = Name(CurrentPhase);
			const bool ProducerDone = Runtime.CharacterControl->GetAttributeValue("ScaleRemoteDone",
				ScriptSecurityContext::CoreTrusted()) == std::optional<WireValue>(WireValue(std::string(PhaseName)));
			const bool AllPeersAcknowledged = Runtime.CharacterControl->GetAttributeValue("ScalePhaseAcks",
				ScriptSecurityContext::CoreTrusted()) == std::optional<WireValue>(WireValue(static_cast<int>(PeerCount)));
			if (AllPeersAcknowledged && !PhaseAcksObserved) {
				PhaseAcksObserved = true;
				std::cout << "[Qualification:Scale] event=phase_acks run=" << RunId << " phase=" << PhaseName
					<< " count=" << PeerCount << " tick=" << Tick << '\n';
			}
			if (ProducerDone && !ProducerAckObserved) {
				ProducerAckObserved = true;
				std::cout << "[Qualification:Scale] event=producer_ack run=" << RunId << " phase=" << PhaseName
					<< " tick=" << Tick << '\n';
			}
			const bool WorldConverged = IsWorldConverged(CurrentPhase);
			if (Tick - PhaseTick >= MinimumPhaseTicks && ProducerDone && AllPeersAcknowledged && WorldConverged) {
				EndPhase(Tick);
				return;
			}
			if (Tick - PhaseTick >= MaximumPhaseTicks) {
				std::cout << "[Qualification:Scale] event=timeout run=" << RunId << " phase=" << PhaseName
					<< " producer_done=" << ProducerDone << " phase_acks=" << AllPeersAcknowledged
					<< " world_converged=" << WorldConverged
					<< " materialization_backlog=" << Session.GetMetrics().MaterializationBacklog << '\n';
				Fail("phase_did_not_converge", Tick);
			}
		}

		[[nodiscard]] bool IsComplete() const { return State == Stage::Complete; }

	  private:
		// These are the existing qualified simulator fixture's 120-tick warmup,
		// 240-tick minimum phase and 1,200-tick phase/setup caps, not new service
		// capacity or latency constants.
		static constexpr std::uint64_t WarmupTicks = 120;
		static constexpr std::uint64_t MinimumPhaseTicks = 240;
		static constexpr std::uint64_t MaximumPhaseTicks = 1'200;
		static constexpr std::uint64_t MaximumSetupTicks = 1'200;
		static constexpr std::uint64_t CompletionPropagationTicks = 120;
		enum class Stage : std::uint8_t { WaitingForPeers, Warming, Measuring, Concluding, Complete };
		enum class Phase : std::uint8_t { Baseline, Load, Resident, Evict, Reload };

		Engine &Runtime;
		network::GameSession &Session;
		std::string RunId;
		Stage State = Stage::WaitingForPeers;
		Phase CurrentPhase = Phase::Baseline;
		std::uint64_t FirstTick = 0;
		std::uint64_t SetupTick = 0;
		std::uint64_t PhaseTick = 0;
		std::uint64_t ConclusionTick = 0;
		std::uint64_t ConvergedWarmupTicks = 0;
		bool PhaseAcksObserved = false;
		bool ProducerAckObserved = false;
		ObjectId FirstContentRoot;
		std::vector<std::shared_ptr<Part>> Grounds;
		std::vector<std::shared_ptr<AnimationTrack>> RootTracks;

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
			throw std::runtime_error("physical scale qualification failed: " + std::string(Reason));
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
			if (Runtime.DataModel->ApplyAttributeMutation("ScalePhase", WireValue(std::string(Name(Value))),
				ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
				Fail("phase_publication_rejected", Tick);
			CurrentPhase = Value;
			PhaseTick = Tick;
			PhaseAcksObserved = false;
			ProducerAckObserved = false;
			State = Stage::Measuring;
			std::cout << "[Qualification:Scale] event=phase_start run=" << RunId << " phase=" << Name(Value)
				<< " tick=" << Tick << " monotonic_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count() << '\n';
		}

		void EndPhase(std::uint64_t Tick) {
			const auto Metrics = Session.GetMetrics();
			const auto Provider = Runtime.Content->GetMetrics();
			const auto Root = GetContentRoot();
			std::cout << "[Qualification:Scale] event=phase_end run=" << RunId << " phase=" << Name(CurrentPhase)
				<< " ticks=" << Tick - PhaseTick << " producer_done=1 phase_acks=" << PeerCount
				<< " observed_objects=" << (Root ? Root->GetDescendants().size() + 1 : 0)
				<< " materialization_backlog=" << Metrics.MaterializationBacklog
				<< " structural_bytes=" << Metrics.StructuralBytesEncoded
				<< " provider_requests=" << Provider.Requests << " provider_acquisitions=" << Provider.Acquisitions
				<< " provider_admissions=" << Provider.Admissions << " provider_evictions=" << Provider.Evictions
				<< " provider_failures=" << Provider.Failures << " tick=" << Tick << '\n';
			if (Provider.Failures != 0) Fail("provider_failure", Tick);
			switch (CurrentPhase) {
			case Phase::Baseline:
				BeginPhase(Phase::Load, Tick);
				break;
			case Phase::Load:
				if (Provider.Acquisitions != 1 || Provider.Admissions != 1 || !Root)
					Fail("shared_first_admission_mismatch", Tick);
				FirstContentRoot = Root->GetObjectId();
				BeginPhase(Phase::Resident, Tick);
				break;
			case Phase::Resident:
				BeginPhase(Phase::Evict, Tick);
				break;
			case Phase::Evict:
				if (Provider.Evictions != 1) Fail("content_eviction_mismatch", Tick);
				BeginPhase(Phase::Reload, Tick);
				break;
			case Phase::Reload:
				if (Provider.Acquisitions != 1 || Provider.Admissions != 2 || !Root ||
					Root->GetObjectId() == FirstContentRoot) Fail("fresh_reload_mismatch", Tick);
				if (Runtime.DataModel->ApplyAttributeMutation("ScalePhase", WireValue(std::string("complete")),
					ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success)
					Fail("completion_publication_rejected", Tick);
				ConclusionTick = Tick;
				State = Stage::Concluding;
				std::cout << "[Qualification:Scale] event=completion_published run=" << RunId
					<< " phase=complete propagation_ticks=" << CompletionPropagationTicks << " tick=" << Tick << '\n';
				break;
			}
		}
	};
} // namespace gargantuan::host
