#include "gargantuan/classes/Player.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/services/Players.hpp"

#include <exception>
#include <stdexcept>

namespace gargantuan {
	namespace {
		void RequireCharacterMutationCapability() {
			if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("Player character lifecycle requires MutateDataModel");
		}

		bool IsEligiblePossession(const Player &Owner, const std::shared_ptr<Instance> &Entity) {
			if (!Entity || Entity->GetDestroyed() || Entity->IsDestroying()) return false;
			const auto DataModelValue = Owner.GetDataModel();
			if (!DataModelValue || Entity->GetDataModel() != DataModelValue) return false;
			const auto WorkspaceValue = DataModelValue->FindFirstChildOfClass("Workspace", std::nullopt);
			for (auto Parent = Entity->GetParent().value_or(nullptr); Parent;
				 Parent = Parent->GetParent().value_or(nullptr)) {
				if (Parent->GetDestroyed() || Parent->IsDestroying()) return false;
				if (Parent == WorkspaceValue) return true;
			}
			return false;
		}

		template <typename Callback>
		void CaptureFirstCallbackFailure(std::exception_ptr &Failure, Callback Invoke) {
			try {
				Invoke();
			} catch (...) {
				if (!Failure) Failure = std::current_exception();
			}
		}

		void ReportCharacterTeardownFailure(const std::exception_ptr &Failure) noexcept {
			if (!Failure) return;
			try {
				std::rethrow_exception(Failure);
			} catch (const std::exception &Error) {
				LOG_ERROR(App, "[Player:Character] Teardown observer failed: %.160s", Error.what());
			} catch (...) {
				LOG_ERROR(App, "[Player:Character] Teardown observer failed");
			}
		}
	}

	Player::Player() {
		Destroying->Connect([this](std::monostate) {
			ClosePossession();
			ShutdownCharacter();
		});
	}

	Player::~Player() {
		DisconnectPossessionSignals();
		ShutdownCharacter();
	}

	void Player::InitializeIdentity(int Value) {
		if (Value <= 0 || PlayerIdValue != 0)
			throw std::logic_error("Player identity must be initialized exactly once");
		PlayerIdValue = Value;
		NotifyPropertyCommitted("PlayerId");
	}

	void Player::SetReplicatedPlayerId(int Value) {
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core)
			throw std::runtime_error("PlayerId is session-owned");
		if (Value <= 0 || (PlayerIdValue != 0 && PlayerIdValue != Value))
			throw std::invalid_argument("PlayerId must be nonzero and immutable");
		if (PlayerIdValue == Value) return;
		PlayerIdValue = Value;
		NotifyPropertyCommitted("PlayerId");
	}

	int Player::GetPlayerId() const {
		return PlayerIdValue;
	}

	std::optional<std::shared_ptr<Instance>> Player::GetPossessedEntity() const {
		if (!PossessedEntityId.IsValid()) return std::nullopt;
		auto Entity = ObjectRegistry::Get().Lookup(PossessedEntityId);
		return Entity && !Entity->GetDestroyed() && !Entity->IsDestroying()
				   ? std::optional(std::move(Entity))
				   : std::nullopt;
	}

	void Player::SetPossession(std::optional<std::shared_ptr<Instance>> Value) {
		const auto &Context = GetCurrentScriptSecurityContext();
		if (!Context.HasCapability(ScriptCapability::MutateDataModel) ||
			(Context.Domain != ScriptExecutionDomain::Server && Context.Domain != ScriptExecutionDomain::Core))
			throw std::runtime_error("[Player:Possession] Assignment requires server authority");
		AssertCanMutate();
		if (PublishingPossessionChange)
			throw std::runtime_error("[Player:Possession] Cannot change possession during its notification");
		const auto Owner = std::dynamic_pointer_cast<Players>(GetParent().value_or(nullptr));
		if (PossessionClosed || PlayerIdValue <= 0 || !Owner)
			throw std::runtime_error("[Player:Possession] Player is outside a live session");
		const auto Entity = Value.value_or(nullptr);
		if (Entity) {
			if (!IsEligiblePossession(*this, Entity))
				throw std::invalid_argument("[Player:Possession] Target must be live in this Workspace");
			const auto EntityId = Entity->GetObjectId();
			for (const auto &Other : Owner->GetPlayers()) {
				if (Other->IsRetiringCharacter(EntityId) ||
					(Other.get() != this &&
					 (Other->GetPossessedEntity().value_or(nullptr) == Entity ||
					  Other->GetCharacter().value_or(nullptr) == Entity || Other->PendingCharacter == EntityId)))
					throw std::invalid_argument("[Player:Possession] Target is already possessed");
			}
		}
		CommitPossession(Entity);
	}

	void Player::SetReplicatedPossessedEntity(std::optional<std::shared_ptr<Instance>> Value) {
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core)
			throw std::runtime_error("[Player:Possession] Reference is published by the server");
		AssertCanMutate();
		const auto Entity = Value.value_or(nullptr);
		if (Entity && (PossessionClosed || !IsEligiblePossession(*this, Entity)))
			throw std::invalid_argument("[Player:Possession] Projected target must be live in this Workspace");
		CommitPossession(Entity);
	}

	void Player::CommitPossession(const std::shared_ptr<Instance> &Entity) {
		const auto Id = Entity ? Entity->GetObjectId() : ObjectId{};
		if (Id == PossessedEntityId) return;
		SignalConnection::Pointer DestroyingConnection;
		SignalConnection::Pointer AncestryConnection;
		if (Entity) {
			const auto WeakPlayer = std::weak_ptr(std::static_pointer_cast<Player>(shared_from_this()));
			DestroyingConnection = Entity->Destroying->Once([WeakPlayer, Id](std::monostate) {
				if (const auto Owner = WeakPlayer.lock(); Owner && Owner->PossessedEntityId == Id)
					Owner->CommitPossession(nullptr);
			});
			try {
				AncestryConnection = Entity->AncestryChanged->Connect([WeakPlayer, Id](const auto &) {
					const auto Owner = WeakPlayer.lock();
					if (Owner && Owner->PossessedEntityId == Id &&
						!IsEligiblePossession(*Owner, ObjectRegistry::Get().Lookup(Id)))
						Owner->CommitPossession(nullptr);
				});
			} catch (...) {
				DestroyingConnection->Disconnect();
				std::erase(Entity->Destroying->Connections, DestroyingConnection);
				throw;
			}
		}
		DisconnectPossessionSignals();
		PossessionDestroyingConnection = std::move(DestroyingConnection);
		PossessionAncestryConnection = std::move(AncestryConnection);
		PossessedEntityValue = Entity;
		PossessedEntityId = Id;
		++PossessionRevision;
		if (!GetDestroyed()) {
			struct NotificationGuard {
				bool &Publishing;
				bool Previous;
				~NotificationGuard() { Publishing = Previous; }
			} Guard{PublishingPossessionChange, PublishingPossessionChange};
			PublishingPossessionChange = true;
			NotifyPropertyCommitted("PossessedEntity");
		}
	}

	void Player::DisconnectPossessionSignals() {
		if (PossessionDestroyingConnection) PossessionDestroyingConnection->Disconnect();
		if (PossessionAncestryConnection) PossessionAncestryConnection->Disconnect();
		if (const auto Entity = PossessedEntityValue.lock()) {
			std::erase(Entity->Destroying->Connections, PossessionDestroyingConnection);
			std::erase(Entity->AncestryChanged->Connections, PossessionAncestryConnection);
		}
	}

	void Player::ClosePossession() {
		PossessionClosed = true;
		CommitPossession(nullptr);
	}

	void Player::InitializeAuthenticationIdentity(PlayerIdentity Identity) {
		ValidatePlayerIdentity(Identity);
		if (AuthenticationIdentity)
			throw std::logic_error("Player authentication identity is immutable once initialized");
		AuthenticationIdentity = std::move(Identity);
	}

	std::optional<std::shared_ptr<Character>> Player::GetCharacter() const {
		return CharacterValue ? std::optional(CharacterValue) : std::nullopt;
	}

	void Player::SetCharacter(std::optional<std::shared_ptr<Character>> Value) {
		RequireCharacterMutationCapability();
		AssertCanMutate();
		ValidatePropertyMutation("Character", Value);
		auto Replacement = Value ? *Value : nullptr;
		if (Replacement == CharacterValue) return;
		if (NotifyingCharacterRemoval || ShuttingDownCharacter)
			throw std::runtime_error("[Player:Character] Cannot change Character during removal");
		ObjectId ReplacementId;
		if (Replacement) {
			if (Replacement->GetDestroyed() || Replacement->IsDestroying())
				throw std::invalid_argument("Player character must be live");
			if (!GetDataModel() || Replacement->GetDataModel() != GetDataModel())
				throw std::invalid_argument("Player character must belong to the same DataModel");
			ReplacementId = Replacement->GetObjectId();
			if (IsRetiringCharacter(ReplacementId))
				throw std::invalid_argument("[Player:Character] Replacement is retiring");
			if (const auto PlayersValue = std::dynamic_pointer_cast<Players>(GetParent().value_or(nullptr))) {
				for (const auto &Child : PlayersValue->Children) {
					auto Other = std::dynamic_pointer_cast<Player>(Child);
					if (Other && Other.get() != this && !Other->GetDestroyed() &&
						(Other->GetCharacter().value_or(nullptr) == Replacement ||
						 Other->GetPossessedEntity().value_or(nullptr) == Replacement ||
						 Other->PendingCharacter == ReplacementId || Other->IsRetiringCharacter(ReplacementId)))
						throw std::invalid_argument("[Player:Possession] Character is already assigned to another Player");
				}
			}
		}

		auto Previous = CharacterValue;
		if (Previous) {
			NotifyCharacterRemoving(Previous, ReplacementId);
			AssertCanMutate();
			if (ShuttingDownCharacter)
				throw std::runtime_error("[Player:Character] Player retired during Character removal");
			if (Replacement && (Replacement->GetDestroyed() || Replacement->IsDestroying() ||
				Replacement->GetDataModel() != GetDataModel()))
				throw std::invalid_argument("[Player:Character] Replacement left the live DataModel during removal");
		}
		SignalConnection::Pointer ReplacementConnection;
		if (Replacement) {
			auto WeakSelf = std::weak_ptr(std::static_pointer_cast<Player>(shared_from_this()));
			ReplacementConnection = Replacement->Destroying->Once(
				[WeakSelf, WeakCharacter = std::weak_ptr(Replacement)](std::monostate) {
					auto Self = WeakSelf.lock();
					auto Character = WeakCharacter.lock();
					if (!Self || !Character || Self->CharacterValue != Character || Self->ShuttingDownCharacter) return;
					std::exception_ptr CallbackFailure;
					if (!Self->NotifyingCharacterRemoval)
						CaptureFirstCallbackFailure(CallbackFailure, [&] { Self->NotifyCharacterRemoving(Character); });
					if (Self->CharacterValue == Character) {
						Self->CharacterValue.reset();
						Self->CharacterDestroyingConnection.reset();
						CaptureFirstCallbackFailure(CallbackFailure, [&] { Self->NotifyPropertyCommitted("Character"); });
					}
					// Instance destruction has already invalidated this Character.
					// An observer failure must not interrupt the remaining teardown.
					ReportCharacterTeardownFailure(CallbackFailure);
				}
			);
		}

		// A committed property observer can start another assignment. Keep the
		// previous Character reserved until its own retirement is complete.
		{
			struct RetirementGuard {
				const CharacterRetirement *&Head;
				CharacterRetirement Entry;
				RetirementGuard(const CharacterRetirement *&Head, ObjectId Target)
					: Head(Head), Entry{Target, Head} { Head = &Entry; }
				~RetirementGuard() { Head = Entry.Previous; }
			} Guard{RetiringCharacters, Previous && !Previous->GetDestroyed() ? Previous->GetObjectId() : ObjectId{}};

			if (CharacterDestroyingConnection) CharacterDestroyingConnection->Disconnect();
			CharacterDestroyingConnection = ReplacementConnection;
			CharacterValue = Replacement;
			std::exception_ptr CallbackFailure;
			CaptureFirstCallbackFailure(CallbackFailure, [&] { NotifyPropertyCommitted("Character"); });
			if (Previous && !Previous->GetDestroyed() && !Previous->IsDestroying())
				CaptureFirstCallbackFailure(CallbackFailure, [&] { Previous->Destroy(); });
			if (CallbackFailure) std::rethrow_exception(CallbackFailure);
		}

		if (ReplacementConnection && CharacterDestroyingConnection == ReplacementConnection &&
			!Replacement->GetDestroyed() && !Replacement->IsDestroying())
			CharacterAdded->Fire(Replacement);
	}

	void Player::LoadCharacter() {
		RequireCharacterMutationCapability();
		AssertCanMutate();
		RemoveCharacter();
		CharacterSpawnRequested->Fire({});
	}

	void Player::ResetCharacter() {
		LoadCharacter();
	}

	void Player::RemoveCharacter() {
		RequireCharacterMutationCapability();
		if (!CharacterValue) return;
		SetCharacter(std::nullopt);
	}

	bool Player::IsRetiringCharacter(ObjectId Target) const {
		for (auto *Retirement = RetiringCharacters; Retirement; Retirement = Retirement->Previous)
			if (Retirement->Target == Target) return true;
		return false;
	}

	void Player::NotifyCharacterRemoving(const std::shared_ptr<Character> &Previous, ObjectId Replacement) {
		struct Reservation {
			Player &Owner;
			~Reservation() {
				Owner.PendingCharacter = {};
				Owner.NotifyingCharacterRemoval = false;
			}
		} Guard{*this};
		PendingCharacter = Replacement;
		NotifyingCharacterRemoval = true;
		CharacterRemoving->Fire(Previous);
	}

	void Player::ShutdownCharacter() {
		if (ShuttingDownCharacter) return;
		ShuttingDownCharacter = true;
		if (CharacterDestroyingConnection) CharacterDestroyingConnection->Disconnect();
		CharacterDestroyingConnection.reset();
		auto Previous = std::move(CharacterValue);
		if (Previous) {
			std::exception_ptr CallbackFailure;
			if (!NotifyingCharacterRemoval)
				CaptureFirstCallbackFailure(CallbackFailure, [&] { NotifyCharacterRemoving(Previous); });
			if (!Previous->GetDestroyed() && !Previous->IsDestroying())
				CaptureFirstCallbackFailure(CallbackFailure, [&] { Previous->Destroy(); });
			ReportCharacterTeardownFailure(CallbackFailure);
		}
	}
}
