#include "gargantuan/runtime/MutationGateway.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	MutationGateway::~MutationGateway() {
		std::deque<PendingMutation> pending;
		{
			std::scoped_lock lock(Mutex);
			pending.swap(Pending);
		}
		for (auto &mutation : pending) {
			mutation.Completion->Complete(
				{MutationStatus::Rejected, std::nullopt, "Mutation gateway shut down before applying command"}
			);
		}
	}

	bool MutationCompletion::IsReady() const {
		std::scoped_lock lock(Mutex);
		return Ready;
	}

	MutationResult MutationCompletion::Wait() {
		std::unique_lock lock(Mutex);
		ReadySignal.wait(lock, [this] { return Ready; });
		return Result;
	}

	void MutationCompletion::Complete(MutationResult result) {
		{
			std::scoped_lock lock(Mutex);
			Result = std::move(result);
			Ready = true;
		}
		ReadySignal.notify_all();
	}

	std::shared_ptr<MutationCompletion> MutationGateway::Submit(
		MutationCommand command,
		ScriptSecurityContext securityContext
	) {
		auto completion = std::make_shared<MutationCompletion>();
		std::scoped_lock lock(Mutex);
		Pending.push_back({std::move(command), completion, std::move(securityContext)});
		return completion;
	}

	MutationResult MutationGateway::Apply(MutationCommand command, ScriptSecurityContext securityContext) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) {
			return {MutationStatus::WrongExecutionDomain, std::nullopt, "Mutation application requires Main"};
		}
		if (!securityContext.HasCapability(ScriptCapability::MutateDataModel))
			return {MutationStatus::Unauthorized, std::nullopt, "Mutation requires MutateDataModel"};

		try {
			return std::visit(
				[&securityContext](auto &typedCommand) -> MutationResult {
					using Command = std::decay_t<decltype(typedCommand)>;
					if constexpr (std::is_same_v<Command, CreateObjectCommand>) {
						auto *definition = InstanceClassRegistry::GetDefinitionByName(typedCommand.ClassName);
						if (!definition || !definition->Constructor)
							return {MutationStatus::InvalidClass, std::nullopt, "Unknown or non-constructible class"};
						if (!typedCommand.Parent)
							return {MutationStatus::Rejected, std::nullopt, "Created objects require an owning parent"};
						std::shared_ptr<Instance> parent;
						if (typedCommand.Parent) {
							parent = ObjectRegistry::Get().Lookup(*typedCommand.Parent);
							if (!parent) return {MutationStatus::StaleObject, std::nullopt, "Parent is stale or dead"};
						}
						auto instance = definition->Constructor();
						if (!instance) return {MutationStatus::InternalError, std::nullopt, "Constructor returned null"};
						const auto id = instance->GetObjectId();
						if (parent) instance->SetParent(parent);
						return {MutationStatus::Success, id, {}};
					} else {
						auto instance = ObjectRegistry::Get().Lookup(typedCommand.Object);
						if (!instance) return {MutationStatus::StaleObject, std::nullopt, "Object is stale or dead"};
						if constexpr (std::is_same_v<Command, UpdatePropertyCommand>) {
							const auto status = instance->ApplyPropertyMutation(
								typedCommand.PropertyName,
								typedCommand.Value,
								Enums::Permission::None,
								securityContext
							);
							return {status, typedCommand.Object, status == MutationStatus::Success ? "" : "Property mutation rejected"};
						} else if constexpr (std::is_same_v<Command, UpdateAttributeCommand>) {
							const auto status = instance->ApplyAttributeMutation(
								typedCommand.AttributeName, std::move(typedCommand.Value), securityContext
							);
							return {status, typedCommand.Object, status == MutationStatus::Success ? "" : "Attribute mutation rejected"};
						} else if constexpr (std::is_same_v<Command, AddTagCommand> || std::is_same_v<Command, RemoveTagCommand>) {
							auto dataModel = instance->GetDataModel();
							if (!dataModel) return {MutationStatus::Rejected, std::nullopt, "Tag target is not owned by a DataModel"};
							const auto scope = dataModel->GetObjectId();
							if constexpr (std::is_same_v<Command, AddTagCommand>)
								(void)dataModel->Tags.Add(scope, typedCommand.Object, typedCommand.TagName, securityContext);
							else
								(void)dataModel->Tags.Remove(scope, typedCommand.Object, typedCommand.TagName, securityContext);
							return {MutationStatus::Success, typedCommand.Object, {}};
						} else if constexpr (std::is_same_v<Command, ReparentObjectCommand>) {
							std::shared_ptr<Instance> parent;
							if (typedCommand.Parent) {
								parent = ObjectRegistry::Get().Lookup(*typedCommand.Parent);
								if (!parent) return {MutationStatus::StaleObject, std::nullopt, "Parent is stale or dead"};
							}
							instance->SetParent(parent ? std::optional(parent) : std::nullopt);
							return {MutationStatus::Success, typedCommand.Object, {}};
						} else {
							instance->Destroy();
							return {MutationStatus::Success, typedCommand.Object, {}};
						}
					}
				},
				command
			);
		} catch (const std::invalid_argument &error) {
			return {MutationStatus::ValidationFailed, std::nullopt, error.what()};
		} catch (const std::exception &error) {
			return {MutationStatus::Rejected, std::nullopt, error.what()};
		} catch (...) {
			return {MutationStatus::InternalError, std::nullopt, "Unknown mutation failure"};
		}
	}

	std::size_t MutationGateway::Drain(std::size_t maximumCommands) {
		AssertAuthoritativeMutation("MutationGateway::Drain");
		std::size_t count = 0;
		while (count < maximumCommands) {
			PendingMutation pending;
			{
				std::scoped_lock lock(Mutex);
				if (Pending.empty()) break;
				pending = std::move(Pending.front());
				Pending.pop_front();
			}
			pending.Completion->Complete(Apply(std::move(pending.Command), std::move(pending.SecurityContext)));
			++count;
		}
		return count;
	}

	std::size_t MutationGateway::GetPendingCount() const {
		std::scoped_lock lock(Mutex);
		return Pending.size();
	}
}
