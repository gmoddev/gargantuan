#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/datatypes/Vector3.hpp"

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
	namespace {
		std::optional<WireValue> ReadAttributeWireValue(lua_State *L, int index) {
			if (lua_isnoneornil(L, index)) return std::nullopt;
			if (lua_isboolean(L, index)) return WireValue(lua_toboolean(L, index) != 0);
			if (lua_isnumber(L, index)) return WireValue(static_cast<double>(lua_tonumber(L, index)));
			if (lua_isstring(L, index)) {
				size_t length = 0;
				const char *value = lua_tolstring(L, index, &length);
				return WireValue(std::string(value, length));
			}
			if (StackValue<Vector2>::Is(L, index)) return EncodeNativeWireValue(StackValue<Vector2>::From(L, index));
			if (StackValue<glm::vec3>::Is(L, index)) return EncodeNativeWireValue(StackValue<glm::vec3>::From(L, index));
			if (StackValue<Color3>::Is(L, index)) return EncodeNativeWireValue(StackValue<Color3>::From(L, index));
			if (StackValue<UDim>::Is(L, index)) return EncodeNativeWireValue(StackValue<UDim>::From(L, index));
			if (StackValue<UDim2>::Is(L, index)) return EncodeNativeWireValue(StackValue<UDim2>::From(L, index));
			if (StackValue<CFrame>::Is(L, index)) return EncodeNativeWireValue(StackValue<CFrame>::From(L, index));
			throw std::invalid_argument("Attribute value type is unsupported");
		}

		int PushAttributeWireValue(lua_State *L, const WireValue &value) {
			return std::visit(
				[L](const auto &typed) -> int {
					using Value = std::decay_t<decltype(typed)>;
					if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, int> ||
						std::is_same_v<Value, double> || std::is_same_v<Value, std::string>)
						return StackValue<Value>::Push(L, typed);
					else if constexpr (std::is_same_v<Value, WireFloat>) return StackValue<float>::Push(L, typed.Value);
					else if constexpr (std::is_same_v<Value, WireVector2>) return StackValue<Vector2>::Push(L, Vector2(typed.X, typed.Y));
					else if constexpr (std::is_same_v<Value, WireVector3>) return StackValue<glm::vec3>::Push(L, {typed.X, typed.Y, typed.Z});
					else if constexpr (std::is_same_v<Value, WireColor3>) return StackValue<Color3>::Push(L, Color3(typed.R, typed.G, typed.B));
					else if constexpr (std::is_same_v<Value, WireUDim>) return StackValue<UDim>::Push(L, UDim(typed.Scale, typed.Offset));
					else if constexpr (std::is_same_v<Value, WireUDim2>)
						return StackValue<UDim2>::Push(L, UDim2(typed.X.Scale, typed.X.Offset, typed.Y.Scale, typed.Y.Offset));
					else if constexpr (std::is_same_v<Value, WireCFrame>) {
						const auto &c = typed.Components;
						return StackValue<CFrame>::Push(L, CFrame(
							glm::vec3(c[0], c[1], c[2]),
							glm::mat3(glm::vec3(c[3], c[6], c[9]), glm::vec3(c[4], c[7], c[10]), glm::vec3(c[5], c[8], c[11]))
						));
					} else throw std::runtime_error("Stored attribute value type is unsupported");
				},
				value
			);
		}

		WireValue EncodeCommittedProperty(Instance *instance, const InstanceProperty &property) {
			if (!property.Read) throw std::runtime_error("Committed property is not readable");
			if (property.ReadObjectReference) {
				auto referenced = property.ReadObjectReference(instance);
				return referenced
					? WireValue(WireObjectReference{WireObjectId::FromObjectId(referenced->GetObjectId())})
					: WireValue(std::monostate{});
			}
			if (property.ReadEnumValue) {
				auto [enumName, enumValue] = property.ReadEnumValue(instance);
				auto enumType = Enums::GetEnums().find(enumName);
				if (enumType == Enums::GetEnums().end()) throw std::runtime_error("Committed enum type is not registered");
				auto item = enumType->second->FromValue(enumValue);
				if (!item) throw std::runtime_error("Committed enum value is not registered");
				return WireEnumItem{enumName, std::string(item->Name)};
			}
			auto encoded = EncodeNativeWireValue(property.Read(instance));
			if (!encoded) throw std::runtime_error("Committed property has no wire encoding");
			return std::move(*encoded);
		}
	}

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

	Instance::Instance() { RequireFrozenRuntimeSchema("Instance construction"); }

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
			const auto scope = definition && definition->ClassName == "DataModel" ? Id : GetReplicationScopeId();
			ChangeJournal::Get().Commit(scope, Id, ObjectCreatedChange{definition ? definition->ClassName : "Instance"});
		}
		return Id;
	}

	void Instance::Destroy() {
		AssertAuthoritativeMutation("Instance::Destroy");
		if (Destroyed || DestroyingState) return;
		DestroyingState = true;
		Destroyed = true;
		const auto objectId = GetObjectId();
		const auto scope = GetReplicationScopeId();
		if (auto dataModel = GetDataModel()) dataModel->Tags.RemoveAll(scope, objectId);
		Attributes.clear();
		AttributeChangedSignals.clear();
		ObjectRegistry::Get().Invalidate(objectId);
		ChangeJournal::Get().Commit(scope, objectId, PropertyUpdatedChange{"Destroyed", true, true});
		GetPropertyChangedSignal("Destroyed")->Fire({});

		Destroying->Fire({});

		auto children = Children;
		for (auto &child : children) {
			child->Destroy();
		}

		SetParent(nullptr);
		ChangeJournal::Get().Commit(scope, objectId, ObjectDestroyedChange{});
	}

	void Instance::AssertIsAlive() const {
		if (Destroyed) throw std::runtime_error("Instance is destroyed");
	}

	void Instance::NotifyPropertyCommitted(std::string_view propertyName) {
		AssertAuthoritativeMutation("Instance property mutation");
		auto *property = FindProperty(std::string(propertyName));
		if (!property) throw std::runtime_error("Committed property does not exist");
		auto committedValue = EncodeCommittedProperty(this, *property);
		const bool replicated = property->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated;
		ChangeJournal::Get().Commit(
			replicated ? GetReplicationScopeId() : ObjectId{},
			GetObjectId(),
			PropertyUpdatedChange{
				std::string(propertyName),
				std::move(committedValue),
				replicated,
			}
		);
		GetPropertyChangedSignal(std::string(propertyName))->Fire({});
	}

	ObjectId Instance::GetReplicationScopeId() const {
		const Instance *root = this;
		std::shared_ptr<Instance> owner;
		while (auto parent = root->ParentReference.lock()) {
			owner = std::move(parent);
			root = owner.get();
		}
		auto *definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(root));
		return definition && definition->ClassName == "DataModel" ? root->GetObjectId() : ObjectId{};
	}

	std::shared_ptr<DataModel> Instance::GetDataModel() const {
		std::shared_ptr<Instance> root = const_cast<Instance *>(this)->shared_from_this();
		while (auto parent = root->ParentReference.lock()) root = std::move(parent);
		return std::dynamic_pointer_cast<DataModel>(root);
	}

	void Instance::PublishReplicationSubtree(ObjectId scope) {
		if (!scope.IsValid()) return;
		auto *definition = InstanceClassRegistry::GetDefinition(this);
		if (!definition) throw std::runtime_error("Replicated object has no class definition");
		const auto objectId = GetObjectId();
		ChangeJournal::Get().Commit(scope, objectId, ObjectCreatedChange{definition->ClassName});
		for (const auto &[name, property] : definition->AllProperties) {
			if (property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
				!property->Read || !property->Write)
				continue;
			ChangeJournal::Get().Commit(
				scope,
				objectId,
				PropertyUpdatedChange{name, EncodeCommittedProperty(this, *property), true}
			);
		}
		for (const auto &[name, value] : Attributes)
			ChangeJournal::Get().Commit(scope, objectId, AttributeUpdatedChange{name, value});
		auto parent = ParentReference.lock();
		ChangeJournal::Get().Commit(
			scope,
			objectId,
			ObjectReparentedChange{parent ? std::optional(parent->GetObjectId()) : std::nullopt}
		);
		if (auto dataModel = GetDataModel()) {
			for (const auto &name : dataModel->Tags.GetTags(scope, objectId, ScriptSecurityContext::CoreTrusted()))
				ChangeJournal::Get().Commit(scope, objectId, TagAddedChange{name});
		}
		for (const auto &child : Children) child->PublishReplicationSubtree(scope);
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
		Enums::Permission permission,
		const ScriptSecurityContext &securityContext
	) {
		auto *property = FindProperty(std::string(propertyName));
		if (!property) return MutationStatus::InvalidProperty;
		if (!property->Write || property->WritePermission == Enums::Permission::Never) return MutationStatus::ReadOnly;
		if (!property->CanWrite(securityContext)) return MutationStatus::Unauthorized;
		if (static_cast<int>(permission) < static_cast<int>(property->WritePermission)) return MutationStatus::Unauthorized;
		if (property->WriteAuthority == InstanceProperty::Authority::Main &&
			GetCurrentExecutionDomain() != ExecutionDomain::Main)
			return MutationStatus::WrongExecutionDomain;
		AssertIsAlive();
		if (property->Validate && !property->Validate(value)) return MutationStatus::ValidationFailed;
		property->Write(this, value);
		return MutationStatus::Success;
	}

	std::optional<WireValue> Instance::GetAttributeValue(
		std::string_view name,
		const ScriptSecurityContext &securityContext
	) const {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Attribute read requires ReadDataModel");
		AssertIsAlive();
		ValidateAttributeName(name);
		auto found = Attributes.find(std::string(name));
		return found == Attributes.end() ? std::nullopt : std::optional(found->second);
	}

	std::map<std::string, WireValue> Instance::GetAttributeValues(const ScriptSecurityContext &securityContext) const {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Attribute read requires ReadDataModel");
		AssertIsAlive();
		return Attributes;
	}

	std::shared_ptr<Signal<std::monostate>> Instance::GetAttributeSignal(
		std::string_view name,
		const ScriptSecurityContext &securityContext
	) {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Attribute signal access requires ReadDataModel");
		AssertIsAlive();
		ValidateAttributeName(name);
		auto [found, inserted] = AttributeChangedSignals.try_emplace(std::string(name));
		if (inserted) found->second = std::make_shared<Signal<std::monostate>>();
		return found->second;
	}

	MutationStatus Instance::ApplyAttributeMutation(
		std::string_view name,
		std::optional<WireValue> value,
		const ScriptSecurityContext &securityContext
	) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return MutationStatus::WrongExecutionDomain;
		if (!securityContext.HasCapability(ScriptCapability::MutateDataModel)) return MutationStatus::Unauthorized;
		AssertIsAlive();
		ValidateAttributeName(name);
		if (value) (void)ValidateAttributeValue(*value);

		auto candidate = Attributes;
		auto found = candidate.find(std::string(name));
		if (value) {
			if (found != candidate.end() && found->second == *value) return MutationStatus::Success;
			candidate[std::string(name)] = *value;
		} else {
			if (found == candidate.end()) return MutationStatus::Success;
			candidate.erase(found);
		}
		(void)ValidateAttributeCollection(candidate);
		Attributes = std::move(candidate);
		ChangeJournal::Get().Commit(GetReplicationScopeId(), GetObjectId(), AttributeUpdatedChange{
			std::string(name), value
		});
		auto signal = AttributeChangedSignals.find(std::string(name));
		if (signal != AttributeChangedSignals.end()) signal->second->Fire({});
		return MutationStatus::Success;
	}

	int Instance::SetAttribute(lua_State *L, Instance *instance) {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
			throw std::runtime_error("Attribute mutation requires MutateDataModel");
		size_t nameLength = 0;
		const char *name = luaL_checklstring(L, 2, &nameLength);
		MutationGateway gateway;
		auto result = gateway.Apply(UpdateAttributeCommand{
			instance->GetObjectId(), std::string(name, nameLength), ReadAttributeWireValue(L, 3)
		});
		if (!result.Succeeded()) throw std::runtime_error(result.Message.empty() ? "Attribute mutation rejected" : result.Message);
		return 0;
	}

	int Instance::GetAttribute(lua_State *L, Instance *instance) {
		size_t nameLength = 0;
		const char *name = luaL_checklstring(L, 2, &nameLength);
		auto value = instance->GetAttributeValue(std::string_view(name, nameLength));
		if (!value) {
			lua_pushnil(L);
			return 1;
		}
		return PushAttributeWireValue(L, *value);
	}

	int Instance::GetAttributes(lua_State *L, Instance *instance) {
		auto attributes = instance->GetAttributeValues();
		lua_createtable(L, 0, static_cast<int>(attributes.size()));
		for (const auto &[name, value] : attributes) {
			PushAttributeWireValue(L, value);
			lua_setfield(L, -2, name.c_str());
		}
		return 1;
	}

	int Instance::GetAttributeChangedSignal(lua_State *L, Instance *instance) {
		size_t nameLength = 0;
		const char *name = luaL_checklstring(L, 2, &nameLength);
		return StackValue<std::shared_ptr<Signal<std::monostate>>>::Push(
			L, instance->GetAttributeSignal(std::string_view(name, nameLength))
		);
	}

	std::string Instance::GetClassName() const {
		AssertIsAlive();
		const InstanceClassDefinition *definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(this));
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
		const auto oldScope = GetReplicationScopeId();
		const auto newScope = newParent ? newParent->GetReplicationScopeId() : ObjectId{};
		const auto objectId = GetObjectId();
		if (!DestroyingState && oldScope != newScope) {
			if (auto oldDataModel = GetDataModel()) oldDataModel->Tags.RemoveAll(oldScope, objectId);
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

		const auto newParentId = newParent ? std::optional(newParent->GetObjectId()) : std::nullopt;
		if (!DestroyingState) {
			if (oldScope == newScope) {
				ChangeJournal::Get().Commit(newScope, objectId, ObjectReparentedChange{newParentId});
			} else {
				if (oldScope.IsValid()) ChangeJournal::Get().Commit(oldScope, objectId, ObjectDestroyedChange{});
				if (newScope.IsValid()) PublishReplicationSubtree(newScope);
			}
		}

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
						if (!property->CanRead(GetCurrentScriptSecurityContext()))
							luaL_error(L, "Current script context cannot read property %s", key);
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
						if (!property->CanWrite(GetCurrentScriptSecurityContext()))
							luaL_error(L, "Current script context cannot write property %s", key);
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
				if (method) {
					if (!method->CanInvoke(GetCurrentScriptSecurityContext()))
						luaL_error(L, "Current script context cannot invoke method %s", key);
					return method->Call(L, self);
				}
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
