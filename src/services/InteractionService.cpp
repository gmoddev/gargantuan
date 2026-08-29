#include "gargantuan/services/InteractionService.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/datatypes/RaycastParams.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <glm/geometric.hpp>

namespace gargantuan {
	namespace {
		constexpr std::string_view InteractionAction = "Interact";

		std::string ReadInteractionRuntime() {
			const auto Candidate = Paths::GetExecutableDirectory() / "runtime" / "DefaultInteractionRuntime.luau";
			std::ifstream Input(Candidate, std::ios::binary);
			if (!Input.is_open())
				throw std::runtime_error(
					"[Interaction:Runtime] Missing shipped Luau module: DefaultInteractionRuntime.luau"
				);
			Input.seekg(0, std::ios::end);
			const auto Size = Input.tellg();
			if (Size < 0 || static_cast<std::size_t>(Size) > MaximumScriptSourceBytes)
				throw std::runtime_error("[Interaction:Runtime] Shipped Luau module exceeds its source bound");
			Input.seekg(0, std::ios::beg);
			std::string Source(static_cast<std::size_t>(Size), '\0');
			Input.read(Source.data(), static_cast<std::streamsize>(Size));
			if (!Input) throw std::runtime_error("[Interaction:Runtime] Failed to read shipped Luau module");
			return Source;
		}

		bool IsFiniteVector(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		float DistanceSquared(const glm::vec3 &Left, const glm::vec3 &Right) {
			const auto Difference = Left - Right;
			return glm::dot(Difference, Difference);
		}
	}

	InteractionService::~InteractionService() {
		ShutdownRuntime();
	}

	std::size_t InteractionService::CellKeyHash::operator()(const CellKey &Value) const noexcept {
		std::size_t Seed = std::hash<std::int64_t>{}(Value.X);
		Seed ^= std::hash<std::int64_t>{}(Value.Y) + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2);
		Seed ^= std::hash<std::int64_t>{}(Value.Z) + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2);
		return Seed;
	}

	std::optional<std::shared_ptr<ProximityPrompt>> InteractionService::GetActivePrompt() const {
		auto Value = std::dynamic_pointer_cast<ProximityPrompt>(ObjectRegistry::Get().Lookup(PresentedPrompt));
		return Value && !Value->GetDestroyed() && !Value->IsDestroying() ? std::optional(Value) : std::nullopt;
	}

	bool InteractionService::GetAvailable() const {
		return PresentedAvailable;
	}

	std::string InteractionService::GetActionText() const {
		return PresentedActionText;
	}

	std::string InteractionService::GetObjectText() const {
		return PresentedObjectText;
	}

	float InteractionService::GetHoldDuration() const {
		return PresentedHoldDuration;
	}

	float InteractionService::GetHoldProgress() const {
		return PresentedHoldProgress;
	}

	std::string InteractionService::GetInputHint() const {
		return "E / A";
	}

	bool InteractionService::GetDefaultPresentationEnabled() const {
		return DefaultPresentationEnabledValue;
	}

	void InteractionService::BeginActivation() {
		SetInputSource(PresentationInputDown, true);
	}

	void InteractionService::EndActivation() {
		SetInputSource(PresentationInputDown, false);
	}

	void InteractionService::SetInputSource(bool &Source, bool Down) {
		if (!RuntimeAttached || Source == Down) return;
		const bool WasDown = SemanticInputDown || PresentationInputDown;
		Source = Down;
		const bool IsDown = SemanticInputDown || PresentationInputDown;
		if (!WasDown && IsDown) ActivationPressed = true;
		if (WasDown && !IsDown) ActivationReleased = true;
	}

	void InteractionService::CancelInput() {
		const bool WasDown = SemanticInputDown || PresentationInputDown;
		SemanticInputDown = false;
		PresentationInputDown = false;
		ActivationPressed = false;
		if (WasDown) ActivationReleased = true;
	}

	void InteractionService::AttachRuntime(
		const std::shared_ptr<DataModel> &WorldValue,
		const std::shared_ptr<Players> &PlayersValue,
		const std::shared_ptr<ActionMap> &ActionMapValue,
		const std::shared_ptr<SemanticSpatialResolver> &SpatialValue,
		bool EnableDefaultPresentation
	) {
		if (RuntimeAttached) return;
		if (!WorldValue || !PlayersValue || !ActionMapValue || !SpatialValue)
			throw std::invalid_argument("InteractionService runtime dependencies must be present");
		World = WorldValue;
		PlayerService = PlayersValue;
		Actions = ActionMapValue;
		SpatialRuntime = SpatialValue;
		PhysicsWorkspace = std::dynamic_pointer_cast<Workspace>(WorldValue->GetService("Workspace"));
		DirtyPrompts.reserve(MaximumPrompts);
		ProcessingDirtyPrompts.reserve(MaximumPrompts);
		DefaultPresentationEnabledValue = EnableDefaultPresentation;
		RuntimeAttached = true;

		auto WeakSelf = std::weak_ptr<InteractionService>(
			std::dynamic_pointer_cast<InteractionService>(shared_from_this())
		);
		ActionBeganConnection = ActionMapValue->ActionBegan->Connect([WeakSelf](std::string ActionName) {
			if (ActionName != InteractionAction) return;
			if (auto Self = WeakSelf.lock()) Self->SetInputSource(Self->SemanticInputDown, true);
		});
		ActionEndedConnection = ActionMapValue->ActionEnded->Connect([WeakSelf](std::string ActionName) {
			if (ActionName != InteractionAction) return;
			if (auto Self = WeakSelf.lock()) Self->SetInputSource(Self->SemanticInputDown, false);
		});

		UnbindDescendants = WorldValue->BindDescendants([WeakSelf](std::shared_ptr<Instance> Value) {
			if (auto Self = WeakSelf.lock())
				if (auto Prompt = std::dynamic_pointer_cast<ProximityPrompt>(Value)) Self->RegisterPrompt(Prompt);
		});
		DescendantRemovedConnection = WorldValue->DescendantRemoved->Connect(
			[WeakSelf](std::shared_ptr<Instance> Value) {
				if (auto Self = WeakSelf.lock())
					if (std::dynamic_pointer_cast<ProximityPrompt>(Value)) Self->UnregisterPrompt(Value->GetObjectId());
			}
		);
	}

	void InteractionService::StartDefaultRuntime() {
		if (!RuntimeAttached || RuntimeScript) return;
		auto Bootstrap = std::make_shared<Script>();
		Bootstrap->SetName("DefaultInteractionRuntime");
		Bootstrap->SetArchivable(false);
		Bootstrap->SetRunContext(Enums::RunContext::Client);
		Bootstrap->SetSource(ReadInteractionRuntime());
		Bootstrap->SetParent(shared_from_this());
		RuntimeScript = std::move(Bootstrap);
	}

	void InteractionService::ShutdownRuntime() {
		if (!RuntimeAttached && !RuntimeScript && Prompts.empty() && PlayerStates.empty()) return;
		RuntimeAttached = false;
		CancelInput();
		if (RuntimeScript && !RuntimeScript->GetDestroyed() && !RuntimeScript->IsDestroying()) RuntimeScript->Destroy();
		RuntimeScript.reset();
		if (ActionBeganConnection) ActionBeganConnection->Disconnect();
		if (ActionEndedConnection) ActionEndedConnection->Disconnect();
		ActionBeganConnection.reset();
		ActionEndedConnection.reset();
		if (UnbindDescendants) UnbindDescendants();
		UnbindDescendants = {};
		if (DescendantRemovedConnection) DescendantRemovedConnection->Disconnect();
		DescendantRemovedConnection.reset();
		for (auto &[_, Record] : Prompts) {
			for (auto &Connection : Record.PromptConnections)
				if (Connection) Connection->Disconnect();
			for (auto &Connection : Record.AnchorConnections)
				if (Connection) Connection->Disconnect();
		}
		Prompts.clear();
		SpatialCells.clear();
		DirtyPrompts.clear();
		ProcessingDirtyPrompts.clear();
		PlayerStates.clear();
		World.reset();
		PlayerService.reset();
		Actions.reset();
		PhysicsWorkspace.reset();
		SpatialRuntime.reset();
		PresentedPrompt = {};
		PresentedAvailable = false;
		PresentedActionText.clear();
		PresentedObjectText.clear();
		PresentedHoldDuration = 0.0f;
		PresentedHoldProgress = 0.0f;
		DefaultPresentationEnabledValue = false;
		SemanticInputDown = false;
		PresentationInputDown = false;
		ActivationPressed = false;
		ActivationReleased = false;
		LastEvaluation = {};
		RejectionLogged = false;
	}

	void InteractionService::RegisterPrompt(const std::shared_ptr<ProximityPrompt> &Prompt) {
		if (!RuntimeAttached || !Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying()) return;
		const auto PromptId = Prompt->GetObjectId();
		if (Prompts.contains(PromptId)) {
			MarkPromptDirty(PromptId);
			return;
		}
		if (Prompts.size() >= MaximumPrompts) {
			if (!RejectionLogged) {
				LOG_WARN(App, "[Interaction:Service] Prompt registration limit reached; additional prompts are inert");
				RejectionLogged = true;
			}
			return;
		}

		auto WeakSelf = std::weak_ptr<InteractionService>(
			std::dynamic_pointer_cast<InteractionService>(shared_from_this())
		);
		PromptRecord Record;
		Record.Prompt = Prompt;
		auto MarkDirty = [WeakSelf, PromptId](std::monostate) {
			if (auto Self = WeakSelf.lock()) Self->MarkPromptDirty(PromptId);
		};
		for (const auto Name :
			 {"Enabled", "ActionText", "ObjectText", "MaxActivationDistance", "HoldDuration", "RequiresLineOfSight"})
			Record.PromptConnections.push_back(Prompt->GetPropertyChangedSignal(Name)->Connect(MarkDirty));
		Record.PromptConnections.push_back(Prompt->AncestryChanged->Connect(
			[WeakSelf, PromptId](std::tuple<std::shared_ptr<Instance>, std::shared_ptr<Instance>>) {
				if (auto Self = WeakSelf.lock()) Self->MarkPromptDirty(PromptId, true);
			}
		));
		Record.PromptConnections.push_back(Prompt->Destroying->Once([WeakSelf, PromptId](std::monostate) {
			if (auto Self = WeakSelf.lock()) Self->UnregisterPrompt(PromptId);
		}));
		Prompts.emplace(PromptId, std::move(Record));
		RefreshPrompt(PromptId);
	}

	void InteractionService::UnregisterPrompt(ObjectId PromptId) {
		auto Found = Prompts.find(PromptId);
		if (Found == Prompts.end()) return;
		RemoveFromCell(Found->second, PromptId);
		for (auto &Connection : Found->second.PromptConnections)
			if (Connection) Connection->Disconnect();
		for (auto &Connection : Found->second.AnchorConnections)
			if (Connection) Connection->Disconnect();
		Prompts.erase(Found);
		std::erase(DirtyPrompts, PromptId);
		InvalidatePromptStates(PromptId);
	}

	void InteractionService::MarkPromptDirty(ObjectId PromptId, bool AnchorDependencies) {
		if (!RuntimeAttached) return;
		auto Found = Prompts.find(PromptId);
		if (Found == Prompts.end()) return;
		Found->second.AnchorDependenciesDirty = Found->second.AnchorDependenciesDirty || AnchorDependencies;
		if (!Found->second.Dirty) {
			Found->second.Dirty = true;
			DirtyPrompts.push_back(PromptId);
		}
		auto Prompt = std::dynamic_pointer_cast<ProximityPrompt>(ObjectRegistry::Get().Lookup(PromptId));
		if (!Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying() || !Prompt->GetEnabled())
			InvalidatePromptStates(PromptId);
	}

	void InteractionService::ProcessDirtyPrompts() {
		if (DirtyPrompts.empty()) return;
		DirtyPrompts.swap(ProcessingDirtyPrompts);
		std::ranges::sort(ProcessingDirtyPrompts);
		for (const auto PromptId : ProcessingDirtyPrompts) {
			if (auto Found = Prompts.find(PromptId); Found != Prompts.end()) Found->second.Dirty = false;
			RefreshPrompt(PromptId);
		}
		ProcessingDirtyPrompts.clear();
	}

	void InteractionService::RemoveFromCell(PromptRecord &Record, ObjectId PromptId) {
		if (!Record.Indexed) return;
		if (auto Cell = SpatialCells.find(Record.Cell); Cell != SpatialCells.end()) {
			std::erase(Cell->second, PromptId);
			if (Cell->second.empty()) SpatialCells.erase(Cell);
		}
		Record.Indexed = false;
	}

	void InteractionService::RefreshPrompt(ObjectId PromptId) {
		auto Found = Prompts.find(PromptId);
		if (Found == Prompts.end()) return;
		auto &Record = Found->second;

		auto Prompt = Record.Prompt.lock();
		if (!Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying() || !Prompt->GetEnabled()) {
			RemoveFromCell(Record, PromptId);
			InvalidatePromptStates(PromptId);
			return;
		}
		std::vector<std::shared_ptr<Instance>> Observed;
		auto Position = ResolveAnchor(Prompt, Record.AnchorDependenciesDirty ? &Observed : nullptr);
		if (!Position) {
			RemoveFromCell(Record, PromptId);
			InvalidatePromptStates(PromptId);
			return;
		}
		auto Cell = PositionToCell(*Position);
		if (!Cell) {
			RemoveFromCell(Record, PromptId);
			InvalidatePromptStates(PromptId);
			return;
		}

		if (Record.AnchorDependenciesDirty) {
			for (auto &Connection : Record.AnchorConnections)
				if (Connection) Connection->Disconnect();
			Record.AnchorConnections.clear();
			auto WeakSelf = std::weak_ptr<InteractionService>(
				std::dynamic_pointer_cast<InteractionService>(shared_from_this())
			);
			for (const auto &Object : Observed) {
				auto ObserveProperty = [&](std::string Name) {
					Record.AnchorConnections.push_back(
						Object->GetPropertyChangedSignal(std::move(Name))->Connect([WeakSelf, PromptId](std::monostate) {
							if (auto Self = WeakSelf.lock()) Self->MarkPromptDirty(PromptId);
						})
					);
				};
				ObserveProperty("CFrame");
				if (std::dynamic_pointer_cast<Attachment>(Object)) {
					ObserveProperty("JointPath");
					ObserveProperty("WorldCFrame");
				} else if (std::dynamic_pointer_cast<BasePart>(Object)) {
					ObserveProperty("Size");
				}
				Record.AnchorConnections.push_back(Object->AncestryChanged->Connect(
					[WeakSelf, PromptId](std::tuple<std::shared_ptr<Instance>, std::shared_ptr<Instance>>) {
						if (auto Self = WeakSelf.lock()) Self->MarkPromptDirty(PromptId, true);
					}
				));
				Record.AnchorConnections.push_back(Object->Destroying->Once([WeakSelf, PromptId](std::monostate) {
					if (auto Self = WeakSelf.lock()) Self->MarkPromptDirty(PromptId, true);
				}));
			}
			Record.AnchorDependenciesDirty = false;
		}
		if (Record.Indexed && Record.Cell != *Cell) RemoveFromCell(Record, PromptId);
		Record.Position = *Position;
		Record.Cell = *Cell;
		if (!Record.Indexed) {
			Record.Indexed = true;
			auto &Bucket = SpatialCells[*Cell];
			auto PositionInBucket = std::ranges::lower_bound(Bucket, PromptId);
			Bucket.insert(PositionInBucket, PromptId);
		}
	}

	void InteractionService::InvalidatePromptStates(ObjectId PromptId) {
		for (auto &[_, State] : PlayerStates) {
			if (State.ActivePrompt == PromptId) State.ActivePrompt = {};
			if (State.HoldingPrompt == PromptId) {
				State.HoldingPrompt = {};
				State.HoldProgress = 0.0f;
			}
		}
	}

	std::optional<InteractionService::CellKey> InteractionService::PositionToCell(const glm::vec3 &Position) {
		if (!IsFiniteVector(Position) || std::abs(Position.x) > MaximumSpatialCoordinate ||
			std::abs(Position.y) > MaximumSpatialCoordinate || std::abs(Position.z) > MaximumSpatialCoordinate)
			return std::nullopt;
		return CellKey{
			static_cast<std::int64_t>(std::floor(static_cast<double>(Position.x) / SpatialCellSize)),
			static_cast<std::int64_t>(std::floor(static_cast<double>(Position.y) / SpatialCellSize)),
			static_cast<std::int64_t>(std::floor(static_cast<double>(Position.z) / SpatialCellSize)),
		};
	}

	std::optional<glm::vec3> InteractionService::ResolveAnchor(
		const std::shared_ptr<ProximityPrompt> &Prompt, std::vector<std::shared_ptr<Instance>> *Observed
	) const {
		if (!Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying()) return std::nullopt;
		auto Transform = SpatialRuntime ? SpatialRuntime->ResolveWorldTransform(Prompt, Observed)
			: SemanticSpatialResolver::ResolveStaticWorldTransform(Prompt);
		if (!Transform || !IsFiniteVector(Transform->WorldCFrame.Position)) return std::nullopt;
		return Transform->WorldCFrame.Position;
	}

	std::optional<glm::vec3> InteractionService::ResolvePlayerOrigin(const std::shared_ptr<Player> &PlayerValue) {
		if (!PlayerValue || PlayerValue->GetDestroyed() || PlayerValue->IsDestroying()) return std::nullopt;
		auto Character = PlayerValue->GetCharacter();
		if (!Character || !*Character || (*Character)->GetDestroyed() || (*Character)->IsDestroying())
			return std::nullopt;
		const auto Position = (*Character)->GetPosition();
		return IsFiniteVector(Position) ? std::optional(Position) : std::nullopt;
	}

	InteractionService::QueryResult
	InteractionService::QueryNearest(const glm::vec3 &Origin, const std::shared_ptr<Player> &PlayerValue) const {
		QueryResult Result;
		auto Center = PositionToCell(Origin);
		if (!Center) return Result;
		constexpr std::int64_t CellRadius = 4;
		struct Candidate {
			ObjectId Prompt;
			float DistanceSquared = 0.0f;
		};
		auto CandidateLess = [](const Candidate &Left, const Candidate &Right) {
			if (Left.DistanceSquared != Right.DistanceSquared) return Left.DistanceSquared < Right.DistanceSquared;
			return Left.Prompt < Right.Prompt;
		};
		std::array<Candidate, MaximumLineOfSightRaycastsPerQuery> Candidates;
		std::size_t CandidateCount = 0;
		for (std::int64_t X = Center->X - CellRadius; X <= Center->X + CellRadius; ++X)
			for (std::int64_t Y = Center->Y - CellRadius; Y <= Center->Y + CellRadius; ++Y)
				for (std::int64_t Z = Center->Z - CellRadius; Z <= Center->Z + CellRadius; ++Z) {
					auto Cell = SpatialCells.find({X, Y, Z});
					if (Cell == SpatialCells.end()) continue;
					for (const auto PromptId : Cell->second) {
						if (Result.Considered == MaximumPromptsConsideredPerQuery) return Result;
						++Result.Considered;
						auto Record = Prompts.find(PromptId);
						if (Record == Prompts.end() || !Record->second.Indexed) continue;
						auto Prompt = Record->second.Prompt.lock();
						if (!Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying() || !Prompt->GetEnabled())
							continue;
						const float CandidateDistanceSquared = DistanceSquared(Origin, Record->second.Position);
						const float Limit = Prompt->GetMaxActivationDistance();
						if (!std::isfinite(CandidateDistanceSquared) || CandidateDistanceSquared > Limit * Limit)
							continue;
						const Candidate Value{PromptId, CandidateDistanceSquared};
						std::size_t InsertAt = 0;
						while (InsertAt < CandidateCount && CandidateLess(Candidates[InsertAt], Value)) ++InsertAt;
						if (InsertAt == Candidates.size()) continue;
						if (CandidateCount < Candidates.size()) ++CandidateCount;
						for (std::size_t Index = CandidateCount - 1; Index > InsertAt; --Index)
							Candidates[Index] = Candidates[Index - 1];
						Candidates[InsertAt] = Value;
					}
				}
		for (std::size_t Index = 0; Index < CandidateCount; ++Index) {
			const auto &Candidate = Candidates[Index];
			auto Found = Prompts.find(Candidate.Prompt);
			auto Prompt = Found == Prompts.end() ? nullptr : Found->second.Prompt.lock();
			if (!Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying() || !Prompt->GetEnabled()) continue;
			if (Prompt->GetRequiresLineOfSight()) {
				if (Result.Raycasts == MaximumLineOfSightRaycastsPerQuery) break;
				++Result.Raycasts;
				if (!PlayerValue || !HasLineOfSight(PlayerValue, Prompt, Origin, Found->second.Position)) continue;
			}
			Result.Prompt = Candidate.Prompt;
			Result.DistanceSquared = Candidate.DistanceSquared;
			break;
		}
		return Result;
	}

	bool InteractionService::HasLineOfSight(
		const std::shared_ptr<Player> &PlayerValue,
		const std::shared_ptr<ProximityPrompt> &Prompt,
		const glm::vec3 &Origin,
		const glm::vec3 &Anchor
	) const {
		if (!Prompt || !Prompt->GetRequiresLineOfSight()) return true;
		auto WorkspaceValue = PhysicsWorkspace.lock();
		if (!WorkspaceValue || WorkspaceValue->GetDestroyed() || WorkspaceValue->IsDestroying()) return false;
		auto Character = PlayerValue ? PlayerValue->GetCharacter() : std::nullopt;
		if (!Character || !*Character || (*Character)->GetDestroyed() || (*Character)->IsDestroying()) return false;
		const auto Direction = Anchor - Origin;
		if (!IsFiniteVector(Direction) ||
			glm::dot(Direction, Direction) < MinimumRaycastDistance * MinimumRaycastDistance)
			return true;
		RaycastParams Params;
		Params.FilterType = Enums::RaycastFilterType::Exclude;
		Params.FilterDescendantsInstances.Values.emplace_back(*Character);
		++LastLineOfSightRaycasts;
		auto Result = WorkspaceValue->ResolveRaycast(Origin, Direction, Params);
		if (!Result.Succeeded()) return false;
		if (!Result.HasHit()) return true;
		auto Current = Prompt->GetParent();
		while (Current && *Current) {
			if (*Current == Result.Instance) return true;
			if (Current->get() == WorkspaceValue.get()) break;
			Current = (*Current)->GetParent();
		}
		return false;
	}

	bool InteractionService::IsActivationValid(
		const std::shared_ptr<Player> &PlayerValue, ObjectId PromptId, std::shared_ptr<ProximityPrompt> *ResolvedPrompt
	) const {
		auto Origin = ResolvePlayerOrigin(PlayerValue);
		if (!Origin) return false;
		auto Found = Prompts.find(PromptId);
		if (Found == Prompts.end() || !Found->second.Indexed) return false;
		auto Prompt = Found->second.Prompt.lock();
		if (!Prompt || Prompt->GetDestroyed() || Prompt->IsDestroying() || !Prompt->GetEnabled()) return false;
		auto Anchor = ResolveAnchor(Prompt);
		if (!Anchor) return false;
		const float CandidateDistanceSquared = DistanceSquared(*Origin, *Anchor);
		const float Limit = Prompt->GetMaxActivationDistance();
		if (!std::isfinite(CandidateDistanceSquared) || CandidateDistanceSquared > Limit * Limit) return false;
		if (!HasLineOfSight(PlayerValue, Prompt, *Origin, *Anchor)) return false;
		if (ResolvedPrompt) *ResolvedPrompt = std::move(Prompt);
		return true;
	}

	bool InteractionService::Trigger(
		const std::shared_ptr<Player> &PlayerValue, PlayerState &State, const std::shared_ptr<ProximityPrompt> &Prompt
	) {
		State.HoldingPrompt = {};
		State.HoldProgress = 1.0f;
		State.AwaitingRelease = true;
		Prompt->Triggered->Fire(PlayerValue);
		return true;
	}

	bool InteractionService::UpdatePlayer(
		const std::shared_ptr<Player> &PlayerValue,
		PlayerState &State,
		Clock::time_point Now,
		bool Pressed,
		bool Released,
		bool InputDown
	) {
		auto Origin = ResolvePlayerOrigin(PlayerValue);
		const auto CandidateQuery = Origin ? QueryNearest(*Origin, PlayerValue) : QueryResult{};
		const auto Candidate = CandidateQuery.Prompt;
		if (State.ActivePrompt != Candidate) {
			State.ActivePrompt = Candidate;
			State.HoldingPrompt = {};
			State.HoldProgress = 0.0f;
		}

		if (Released) {
			State.AwaitingRelease = false;
			State.HoldingPrompt = {};
			State.HoldProgress = 0.0f;
		}

		if (Pressed && !State.AwaitingRelease && State.ActivePrompt.IsValid()) {
			std::shared_ptr<ProximityPrompt> Prompt;
			if (IsActivationValid(PlayerValue, State.ActivePrompt, &Prompt)) {
				if (Prompt->GetHoldDuration() <= 0.0f) return Trigger(PlayerValue, State, Prompt);
				State.HoldingPrompt = State.ActivePrompt;
				State.HoldStarted = Now;
				State.HoldProgress = 0.0f;
			}
		}

		if (!State.HoldingPrompt.IsValid()) return false;
		if (!InputDown || State.HoldingPrompt != State.ActivePrompt) {
			State.HoldingPrompt = {};
			State.HoldProgress = 0.0f;
			return false;
		}
		std::shared_ptr<ProximityPrompt> Prompt;
		if (!IsActivationValid(PlayerValue, State.HoldingPrompt, &Prompt)) {
			State.HoldingPrompt = {};
			State.HoldProgress = 0.0f;
			return false;
		}
		const float Duration = Prompt->GetHoldDuration();
		if (Duration <= 0.0f) return Trigger(PlayerValue, State, Prompt);
		const float Elapsed = std::chrono::duration<float>(Now - State.HoldStarted).count();
		State.HoldProgress = std::clamp(Elapsed / Duration, 0.0f, 1.0f);
		if (Elapsed >= Duration) return Trigger(PlayerValue, State, Prompt);
		return false;
	}

	void InteractionService::Step(Clock::time_point Now) {
		if (!RuntimeAttached) return;
		LastLineOfSightRaycasts = 0;
		const bool HasPendingInput = ActivationPressed || ActivationReleased;
		if (!HasPendingInput && DirtyPrompts.empty() && LastEvaluation.time_since_epoch().count() != 0 &&
			Now >= LastEvaluation && Now - LastEvaluation < EvaluationInterval)
			return;
		LastEvaluation = Now;
		ProcessDirtyPrompts();
		const bool Pressed = std::exchange(ActivationPressed, false);
		const bool Released = std::exchange(ActivationReleased, false);
		const bool InputDown = SemanticInputDown || PresentationInputDown;

		auto PlayersValue = PlayerService.lock();
		if (!PlayersValue) return;
		auto CurrentPlayers = PlayersValue->GetPlayers();
		if (CurrentPlayers.size() > MaximumPlayers) CurrentPlayers.resize(MaximumPlayers);
		std::unordered_set<ObjectId> LivePlayers;
		LivePlayers.reserve(CurrentPlayers.size());
		auto LocalPlayer = PlayersValue->GetLocalPlayer();
		const auto LocalId = LocalPlayer ? (*LocalPlayer)->GetObjectId() : ObjectId{};
		for (const auto &PlayerValue : CurrentPlayers) {
			if (!PlayerValue || PlayerValue->GetDestroyed() || PlayerValue->IsDestroying()) continue;
			const auto PlayerId = PlayerValue->GetObjectId();
			LivePlayers.insert(PlayerId);
			auto [State, _] = PlayerStates.try_emplace(PlayerId);
			const bool IsLocal = PlayerId == LocalId;
			if (UpdatePlayer(
					PlayerValue, State->second, Now, IsLocal && Pressed, IsLocal && Released, IsLocal && InputDown
				) &&
				!RuntimeAttached)
				return;
		}
		std::erase_if(PlayerStates, [&](const auto &Entry) { return !LivePlayers.contains(Entry.first); });
		PublishLocalPresentation();
	}

	void InteractionService::PublishLocalPresentation() {
		auto PlayersValue = PlayerService.lock();
		if (!PlayersValue) return;
		auto LocalPlayer = PlayersValue->GetLocalPlayer();
		ObjectId PromptId;
		float Progress = 0.0f;
		if (LocalPlayer) {
			if (auto State = PlayerStates.find((*LocalPlayer)->GetObjectId()); State != PlayerStates.end()) {
				PromptId = State->second.ActivePrompt;
				Progress = State->second.HoldProgress;
			}
		}
		auto Prompt = std::dynamic_pointer_cast<ProximityPrompt>(ObjectRegistry::Get().Lookup(PromptId));
		const bool Available = Prompt && !Prompt->GetDestroyed() && !Prompt->IsDestroying() && Prompt->GetEnabled();
		const auto ActionText = Available ? Prompt->GetActionText() : std::string{};
		const auto ObjectText = Available ? Prompt->GetObjectText() : std::string{};
		const float HoldDuration = Available ? Prompt->GetHoldDuration() : 0.0f;
		if (!Available) {
			PromptId = {};
			Progress = 0.0f;
		}
		const bool Changed = PresentedPrompt != PromptId || PresentedAvailable != Available ||
							 PresentedActionText != ActionText || PresentedObjectText != ObjectText ||
							 PresentedHoldDuration != HoldDuration || PresentedHoldProgress != Progress;
		PresentedPrompt = PromptId;
		PresentedAvailable = Available;
		PresentedActionText = ActionText;
		PresentedObjectText = ObjectText;
		PresentedHoldDuration = HoldDuration;
		PresentedHoldProgress = Progress;
		if (Changed) PresentationChanged->Fire({});
	}
}
