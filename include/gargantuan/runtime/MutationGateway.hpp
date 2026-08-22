#pragma once

#include "gargantuan/runtime/AuthoritativeTransactions.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/WireValue.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <any>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace gargantuan {
	inline constexpr std::size_t MaximumPendingMutations = 4096;

	enum class MutationCommandOrigin { LocalInternal, Studio, AuthenticatedPeer };

	class MutationAuthorityContext {
	  public:
		[[nodiscard]] static MutationAuthorityContext Local(ScriptSecurityContext SecurityContext);
		[[nodiscard]] static MutationAuthorityContext Studio(
			ScriptSecurityContext SecurityContext,
			ObjectId Scope,
			std::optional<TransactionId> Transaction = std::nullopt,
			std::uint64_t TransactionOwner = 1
		);
		[[nodiscard]] static MutationAuthorityContext AuthenticatedPeer(
			ScriptSecurityContext SecurityContext,
			ObjectId Scope
		);

		[[nodiscard]] const ScriptSecurityContext &GetSecurityContext() const { return SecurityContext; }
		[[nodiscard]] MutationCommandOrigin GetOrigin() const { return Origin; }
		[[nodiscard]] std::optional<ObjectId> GetScope() const { return Scope; }
		[[nodiscard]] std::optional<TransactionId> GetTransactionId() const { return Transaction; }
		[[nodiscard]] std::uint64_t GetTransactionOwner() const { return TransactionOwner; }

	  private:
		MutationAuthorityContext(
			MutationCommandOrigin OriginValue,
			ScriptSecurityContext SecurityContextValue,
			std::optional<ObjectId> ScopeValue,
			std::optional<TransactionId> TransactionValue = std::nullopt,
			std::uint64_t TransactionOwnerValue = 0
		);

		MutationCommandOrigin Origin;
		ScriptSecurityContext SecurityContext;
		std::optional<ObjectId> Scope;
		std::optional<TransactionId> Transaction;
		std::uint64_t TransactionOwner = 0;
	};

	struct CreateObjectCommand {
		SchemaId ClassSchemaId;
		std::uint32_t DefinitionVersion = 0;
		ObjectId Parent;
		std::optional<std::string> Name;
	};
	struct UpdatePropertyCommand { ObjectId Object; std::string PropertyName; std::any Value; };
	struct UpdateWirePropertyCommand { ObjectId Object; std::string PropertyName; WireValue Value; };
	struct UpdateScriptSourceCommand {
		ObjectId Object;
		int ExpectedSourceVersion = 0;
		std::string Source;
	};
	struct UpdateAttributeCommand { ObjectId Object; std::string AttributeName; std::optional<WireValue> Value; };
	struct UpdateExtensionPropertyCommand {
		ObjectId Object;
		SchemaId ExtensionSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::string PropertyName;
		WireValue Value;
	};
	struct AddTagCommand { ObjectId Object; std::string TagName; std::optional<ObjectId> ExpectedScope; };
	struct RemoveTagCommand { ObjectId Object; std::string TagName; std::optional<ObjectId> ExpectedScope; };
	struct ReparentObjectCommand { ObjectId Object; std::optional<ObjectId> Parent; };
	struct DestroyObjectCommand { ObjectId Object; };
	struct DuplicateObjectCommand { ObjectId Object; };
	struct RestoreSubtreeCommand {
		std::string PersistentSnapshot;
		ObjectId Parent;
		std::size_t ExpectedObjectCount = 0;
	};
	using MutationCommand = std::variant<
		CreateObjectCommand,
		UpdatePropertyCommand,
		UpdateWirePropertyCommand,
		UpdateScriptSourceCommand,
		UpdateAttributeCommand,
		UpdateExtensionPropertyCommand,
		AddTagCommand,
		RemoveTagCommand,
		ReparentObjectCommand,
		DestroyObjectCommand,
		DuplicateObjectCommand,
		RestoreSubtreeCommand
	>;

	enum class MutationStatus {
		Success,
		WrongExecutionDomain,
		StaleObject,
		InvalidClass,
		InvalidProperty,
		InvalidParent,
		ProtectedObject,
		ResourceLimit,
		RevisionExhausted,
		ReadOnly,
		Unauthorized,
		ValidationFailed,
		Conflict,
		Rejected,
		InternalError,
		TransactionNotFound,
		TransactionLimit,
	};
	[[nodiscard]] const char *GetMutationStatusDescription(MutationStatus Status);

	struct MutationResult {
		MutationStatus Status = MutationStatus::InternalError;
		std::optional<ObjectId> Object;
		std::string Message;
		[[nodiscard]] bool Succeeded() const { return Status == MutationStatus::Success; }
	};

	class MutationCompletion {
	  public:
		[[nodiscard]] bool IsReady() const;
		MutationResult Wait();

	  private:
		friend class MutationGateway;
		void Complete(MutationResult result);
		mutable std::mutex Mutex;
		std::condition_variable ReadySignal;
		bool Ready = false;
		MutationResult Result;
	};

	class MutationGateway {
	  public:
		~MutationGateway();
		std::shared_ptr<MutationCompletion> Submit(
			MutationCommand command,
			ScriptSecurityContext securityContext = GetCurrentScriptSecurityContext()
		);
		std::shared_ptr<MutationCompletion> Submit(
			MutationCommand Command,
			MutationAuthorityContext Authority
		);
		MutationResult Apply(
			MutationCommand command,
			ScriptSecurityContext securityContext = GetCurrentScriptSecurityContext()
		);
		MutationResult Apply(MutationCommand Command, MutationAuthorityContext Authority);
		TransactionResult Undo(DataModel &World, ScriptSecurityContext SecurityContext);
		TransactionResult Redo(DataModel &World, ScriptSecurityContext SecurityContext);
		std::size_t Drain(std::size_t maximumCommands = static_cast<std::size_t>(-1));
		[[nodiscard]] std::size_t GetPendingCount() const;

	  private:
		struct PendingMutation {
			MutationCommand Command;
			std::shared_ptr<MutationCompletion> Completion;
			MutationAuthorityContext Authority;
		};
		mutable std::mutex Mutex;
		std::deque<PendingMutation> Pending;
	};
}
