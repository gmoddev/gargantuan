#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	G_USERDATA_IMPL(
		Instance,
		.Tag = UserdataTag::Instance,
		.Type = "Instance",
		.Methods = {
			{"__index", Method{&Instance::LIndex}},
			{"__newindex", Method{&Instance::LNewIndex}},
			{"__namecall", Method{&Instance::LNamecall}},
		}
	);

	Instance::Instance() = default;

	Instance::~Instance() {
		ObjectRegistry::Get().Invalidate(Id);
	}

	ObjectId Instance::GetObjectId() const {
		std::scoped_lock lock(IdentityMutex);
		if (!Id.IsValid()) {
			AssertAuthoritativeMutation("Instance identity publication");
			auto self = const_cast<Instance *>(this)->shared_from_this();
			Id = ObjectRegistry::Get().Register(self);
			auto definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(this));
			ChangeJournal::Get().Commit(Id, ObjectCreatedChange{definition ? definition->ClassName : "Instance"});
		}
		return Id;
	}

	void Instance::Destroy() {
		AssertAuthoritativeMutation("Instance::Destroy");
		if (Destroyed || DestroyingState) return;
		DestroyingState = true;
		Destroyed = true;
		const auto objectId = GetObjectId();
		ObjectRegistry::Get().Invalidate(objectId);
		ChangeJournal::Get().Commit(objectId, PropertyUpdatedChange{"Destroyed", true});
		GetPropertyChangedSignal("Destroyed")->Fire({});

		Destroying->Fire({});

		auto children = Children;
		for (auto &child : children) {
			child->Destroy();
		}

		SetParent(nullptr);
		ChangeJournal::Get().Commit(objectId, ObjectDestroyedChange{});
	}

	void Instance::AssertIsAlive() const {
		if (Destroyed) throw std::runtime_error("Instance is destroyed");
	}

	void Instance::NotifyPropertyCommitted(std::string_view propertyName) {
		AssertAuthoritativeMutation("Instance property mutation");
		auto *property = FindProperty(std::string(propertyName));
		std::any committedValue;
		if (property && property->Read) committedValue = property->Read(this);
		ChangeJournal::Get().Commit(
			GetObjectId(), PropertyUpdatedChange{std::string(propertyName), std::move(committedValue)}
		);
		GetPropertyChangedSignal(std::string(propertyName))->Fire({});
	}

	void Instance::AssertCanMutate() const {
		AssertAuthoritativeMutation("Instance property mutation");
		AssertIsAlive();
	}

	void Instance::ValidatePropertyMutation(std::string_view propertyName, const std::any &value) const {
		auto *property = const_cast<Instance *>(this)->FindProperty(std::string(propertyName));
		if (!property) throw std::invalid_argument("Property does not exist");
		if (property->Validate && !property->Validate(value)) throw std::invalid_argument("Property validation failed");
	}

	MutationStatus Instance::ApplyPropertyMutation(
		std::string_view propertyName,
		const std::any &value,
		Enums::Permission permission
	) {
		auto *property = FindProperty(std::string(propertyName));
		if (!property) return MutationStatus::InvalidProperty;
		if (!property->Write || property->WritePermission == Enums::Permission::Never) return MutationStatus::ReadOnly;
		if (static_cast<int>(permission) < static_cast<int>(property->WritePermission)) return MutationStatus::Unauthorized;
		if (property->WriteAuthority == InstanceProperty::Authority::Main &&
			GetCurrentExecutionDomain() != ExecutionDomain::Main)
			return MutationStatus::WrongExecutionDomain;
		AssertIsAlive();
		if (property->Validate && !property->Validate(value)) return MutationStatus::ValidationFailed;
		property->Write(this, value);
		return MutationStatus::Success;
	}

	std::string Instance::GetClassName() const {
		AssertIsAlive();
		InstanceClassDefinition *definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(this));
		return definition->ClassName;
	}

	void Instance::FireAncestryChanged(std::shared_ptr<Instance> child, std::shared_ptr<Instance> parent) {
		AncestryChanged->Fire({child, parent});
		GetPropertyChangedSignal("Parent")->Fire({});
		for (auto &descendant : Children) {
			descendant->FireAncestryChanged(child, parent);
		}
	}

	std::optional<std::shared_ptr<Instance>> Instance::GetParent() const {
		if (auto parent = ParentReference.lock()) return parent;
		return std::nullopt;
	}

	void Instance::SetParent(std::optional<std::shared_ptr<Instance>> value) {
		AssertAuthoritativeMutation("Instance::SetParent");
		if (!DestroyingState) AssertIsAlive();

		std::shared_ptr<Instance> newParent = value.has_value() ? value.value() : nullptr;
		if (DestroyingState && newParent) throw std::runtime_error("A destroying Instance cannot be reparented");
		if (newParent) newParent->AssertIsAlive();
		auto oldParent = ParentReference.lock();
		if (oldParent == newParent) return;

		std::shared_ptr<Instance> self = shared_from_this();
		if (newParent == self) throw std::invalid_argument("An Instance cannot be parented to itself");
		for (auto ancestor = newParent; ancestor; ancestor = ancestor->ParentReference.lock()) {
			if (ancestor == self) throw std::invalid_argument("An Instance cannot be parented to its descendant");
		}

		// This whole subtree leaves the old ancestry and joins the new one, so
		// collect it once up front and reuse it for both sets of signals
		std::vector<std::shared_ptr<Instance>> subtree = {self};
		CollectDescendants(subtree);

		if (oldParent != nullptr) {
			auto &oldChildren = oldParent->Children;
			if (auto it = std::find(oldChildren.begin(), oldChildren.end(), self); it != oldChildren.end()) {
				oldChildren.erase(it);
			}
		}

		ParentReference = newParent;

		if (newParent != nullptr) {
			newParent->Children.push_back(self);
		}

		const auto objectId = GetObjectId();
		const auto newParentId = newParent ? std::optional(newParent->GetObjectId()) : std::nullopt;
		ChangeJournal::Get().Commit(objectId, ObjectReparentedChange{newParentId});

		if (oldParent != nullptr) {
			oldParent->ChildRemoved->Fire(self);
			for (auto ancestor = oldParent; ancestor; ancestor = ancestor->ParentReference.lock()) {
				for (auto &node : subtree) ancestor->DescendantRemoved->Fire(node);
			}
		}

		if (newParent != nullptr) {
			newParent->ChildAdded->Fire(self);

			for (auto ancestor = newParent; ancestor; ancestor = ancestor->ParentReference.lock()) {
				for (auto &node : subtree) ancestor->DescendantAdded->Fire(node);
			}
		}

		FireAncestryChanged(self, newParent);
	}

	void Instance::ClearAllChildren() {
		auto children = Children;
		for (auto &child : children) {
			child->Destroy();
		}
	}

	std::function<void()> Instance::BindChildren(std::function<void(std::shared_ptr<Instance> inst)> callback) {
		for (auto &child : Children) {
			callback(child);
		}

		auto conn = ChildAdded->Connect(callback);
		return [conn]() { conn->Disconnect(); };
	}

	std::function<void()> Instance::BindDescendants(std::function<void(std::shared_ptr<Instance> inst)> callback) {
		for (auto &child : GetDescendants()) {
			callback(child);
		}

		auto conn = DescendantAdded->Connect(callback);
		return [conn]() { conn->Disconnect(); };
	}

	bool Instance::IsPropertyModified(std::string propertyName) {
		return true;
		// auto property = FindProperty(propertyName);

		// if (!property) throw std::runtime_error("Property does not exist");
		// if (property->Signal) throw std::runtime_error("Property is a signal");
		// if (!property->Read || property->ReadPermission == Enums::Permission::Never) {
		// 	throw std::runtime_error("Property is read-only");
		// };

		// return property->Read(this) != property->Unmodified;
	};

	std::shared_ptr<Signal<std::monostate>> Instance::GetPropertyChangedSignal(std::string propertyName) {
		if (PropertyChangedSignals.contains(propertyName)) return PropertyChangedSignals[propertyName];

		auto signal = std::make_shared<Signal<std::monostate>>();
		auto property = FindProperty(propertyName);

		if (!property) throw std::runtime_error("Property does not exist");
		PropertyChangedSignals.emplace(propertyName, signal);
		return signal;
	};

	void Instance::ResetPropertyToDefault(std::string propertyName) {
		auto *property = FindProperty(propertyName);
		if (!property || property->Signal) throw std::runtime_error("Property does not exist or is a signal");
		const auto status = ApplyPropertyMutation(propertyName, property->Unmodified, Enums::Permission::Engine);
		if (status != MutationStatus::Success) throw std::runtime_error("Default property mutation rejected");
	};

	const InstanceProperty *Instance::FindProperty(std::string name) {
		const InstanceClassDefinition *definition = InstanceClassRegistry::GetDefinition(this);
		if (!definition) return nullptr;

		auto it = definition->AllProperties.find(name);
		return it != definition->AllProperties.end() ? it->second : nullptr;
	}

	const Instance::Self::Method *Instance::FindMethod(std::string name) {
		const InstanceClassDefinition *definition = InstanceClassRegistry::GetDefinition(this);
		if (!definition) return nullptr;

		auto it = definition->AllMethods.find(name);
		return it != definition->AllMethods.end() ? it->second : nullptr;
	}

	int Instance::LIndex(lua_State *L, Instance *self) {
		return InvokeNativeCallback(L, [L, self] {
			const char *key = luaL_checkstring(L, 2);

			if (key && self) {
				const auto *property = self->FindProperty(key);
				if (property) {
					if (property->Read) {
						return property->PushStack(L, property->Read(self));
					} else {
						luaL_error(L, "Property %s is write-only", key);
					}
				} else if (auto child = self->FindFirstChild(key, std::nullopt)) {
					StackValue<std::shared_ptr<Instance>>::Push(L, child);
					return 1;
				}
			}

			return 0;
		});
	};

	int Instance::LNewIndex(lua_State *L, Instance *self) {
		return InvokeNativeCallback(L, [L, self] {
			const char *key = luaL_checkstring(L, 2);

			if (key && self) {
				const auto *property = self->FindProperty(key);
				if (property) {
					if (property->Write && property->WritePermission != Enums::Permission::Never) {
						if (!property->IsStack(L, 3)) luaL_typeerrorL(L, 3, property->ReflectedTypedef.c_str());
						auto value = property->FromStack(L, 3);
						const auto status = self->ApplyPropertyMutation(key, value);
						if (status != MutationStatus::Success) throw std::runtime_error("Property mutation rejected");
						return 0;
					} else {
						luaL_error(L, "Property %s is read-only", key);
					}
				}
			}

			luaL_error(L, "Unknown property %s", key);

			return 0;
		});
	};

	int Instance::LNamecall(lua_State *L, Instance *self) {
		return InvokeNativeCallback(L, [L, self] {
			const char *key = lua_namecallatom(L, nullptr);

			if (key && self) {
				const auto *method = self->FindMethod(key);
				if (method) return method->Call(L, self);
			}

			luaL_error(L, "%s is not a valid method of %s", key, self->Name.data());
			return 0;
		});
	};

	std::string Instance::GetFullName() {
		std::vector<std::string_view> path;

		size_t totalLength = 0;
		std::shared_ptr<Instance> owner;
		Instance *current = this;

		while (current) {
			auto &name = current->Name;
			path.push_back(name);
			totalLength += name.size() + 1;
			owner = current->ParentReference.lock();
			current = owner.get();
		};

		if (path.empty()) {
			return "";
		}

		if (totalLength > 0) {
			totalLength--;
		}

		std::string fullName;
		fullName.reserve(totalLength);

		auto begin = path.rbegin();
		for (auto it = begin; it != path.rend(); ++it) {
			if (it != begin) {
				fullName.push_back('.');
			}
			fullName.append(*it);
		}

		return fullName;
	};

	bool Instance::IsA(std::string className) {
		auto currentDefinition = InstanceClassRegistry::GetDefinition(this);
		while (true) {
			if (currentDefinition->ClassName == className) {
				return true;
			}

			auto superclass = currentDefinition->Superclass;
			if (superclass.has_value()) {
				currentDefinition = InstanceClassRegistry::GetDefinitionByName(superclass.value());
			} else {
				return false;
			}
		}
	}

	std::vector<std::shared_ptr<Instance>> Instance::GetChildren() {
		return Children;
	}

	void Instance::CollectDescendants(std::vector<std::shared_ptr<Instance>> &descendants) {
		for (const auto &child : Children) {
			descendants.push_back(child);
			child->CollectDescendants(descendants);
		}
	}

	std::vector<std::shared_ptr<Instance>> Instance::GetDescendants() {
		std::vector<std::shared_ptr<Instance>> descendants;
		CollectDescendants(descendants);
		return descendants;
	}

	std::shared_ptr<Instance> Instance::FindFirstChild(std::string name, std::optional<bool> recursive) {
		for (const auto &child : Children) {
			if (child->Name == name) return child;
			if (recursive) {
				if (auto found = child->FindFirstChild(name, recursive)) return found;
			}
		};
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstChildOfClass(std::string className, std::optional<bool> recursive) {
		for (const auto &child : Children) {
			if (InstanceClassRegistry::GetDefinition(child.get())->ClassName == className) return child;
			if (recursive) {
				if (auto found = child->FindFirstChildOfClass(className, recursive)) return found;
			}
		};
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstChildWhichIsA(std::string className, std::optional<bool> recursive) {
		for (const auto &child : Children) {
			if (InstanceClassRegistry::GetDefinition(child.get())->InheritedClasses.contains(className)) return child;
			if (recursive) {
				if (auto found = child->FindFirstChildWhichIsA(className, recursive)) return found;
			}
		};
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstDescendant(std::string name) {
		return FindFirstChild(name, true);
	}

	std::shared_ptr<Instance> Instance::FindFirstDescendantOfClass(std::string className) {
		return FindFirstChildOfClass(className, true);
	}

	std::shared_ptr<Instance> Instance::FindFirstDescendantWhichIsA(std::string className) {
		return FindFirstChildWhichIsA(className, true);
	}

	std::shared_ptr<Instance> Instance::FindFirstAncestor(std::string name) {
		auto current = ParentReference.lock();
		while (current) {
			if (current->Name == name) return current;
			current = current->ParentReference.lock();
		}
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstAncestorOfClass(std::string className) {
		auto current = ParentReference.lock();
		while (current) {
			if (InstanceClassRegistry::GetDefinition(current.get())->ClassName == className) {
				return current;
			}
			current = current->ParentReference.lock();
		}
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstAncestorWhichIsA(std::string className) {
		auto current = ParentReference.lock();
		while (current) {
			if (InstanceClassRegistry::GetDefinition(current.get())->InheritedClasses.contains(className)) {
				return current;
			}
			current = current->ParentReference.lock();
		}
		return nullptr;
	}
}
