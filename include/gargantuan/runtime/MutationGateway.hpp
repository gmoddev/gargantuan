#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

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
	struct CreateObjectCommand { std::string ClassName; std::optional<ObjectId> Parent; };
	struct UpdatePropertyCommand { ObjectId Object; std::string PropertyName; std::any Value; };
	struct ReparentObjectCommand { ObjectId Object; std::optional<ObjectId> Parent; };
	struct DestroyObjectCommand { ObjectId Object; };
	using MutationCommand = std::variant<CreateObjectCommand, UpdatePropertyCommand, ReparentObjectCommand, DestroyObjectCommand>;

	enum class MutationStatus {
		Success,
		WrongExecutionDomain,
		StaleObject,
		InvalidClass,
		InvalidProperty,
		ReadOnly,
		Unauthorized,
		ValidationFailed,
		Rejected,
		InternalError,
	};

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
		std::shared_ptr<MutationCompletion> Submit(MutationCommand command);
		MutationResult Apply(MutationCommand command);
		std::size_t Drain(std::size_t maximumCommands = static_cast<std::size_t>(-1));
		[[nodiscard]] std::size_t GetPendingCount() const;

	  private:
		struct PendingMutation {
			MutationCommand Command;
			std::shared_ptr<MutationCompletion> Completion;
		};
		mutable std::mutex Mutex;
		std::deque<PendingMutation> Pending;
	};
}
