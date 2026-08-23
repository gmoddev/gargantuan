#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
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
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gargantuan {
	namespace {
		std::string InstanceClassName(const Instance *InstanceValue) {
			auto *Definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(InstanceValue));
			if (!Definition) return "Instance";
			return Definition->ConstructionKind == SchemaClassConstructionKind::CustomData
				? Definition->CanonicalName : Definition->ClassName;
		}
		std::shared_ptr<Instance> ResolveServiceInstance(Instance *InstanceValue, std::string_view Name) {
			auto *Provider = dynamic_cast<ServiceProvider *>(InstanceValue);
			if (!Provider) return nullptr;
			auto Service = Provider->ResolveService(Name);
			return Service ? *Service : nullptr;
		}
		bool IsNativeDataModelDefinition(const SchemaClassDefinition *definition) {
			static const auto DataModelId = SchemaId::FromNativeName("Engine", "DataModel");
			return definition && definition->Id == DataModelId;
		}

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

		WireValue ReadExtensionWireValue(
			lua_State *L,
			int index,
			SchemaExtensionPropertyType type
		) {
			switch (type) {
				case SchemaExtensionPropertyType::Boolean:
					if (lua_type(L, index) != LUA_TBOOLEAN) luaL_typeerrorL(L, index, "boolean");
					return lua_toboolean(L, index) != 0;
				case SchemaExtensionPropertyType::Integer: {
					if (lua_type(L, index) != LUA_TNUMBER) luaL_typeerrorL(L, index, "integer");
					const auto value = lua_tonumber(L, index);
					if (!std::isfinite(value) || std::trunc(value) != value ||
						value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
						throw std::invalid_argument("Extension Integer value must be a signed 32-bit integer");
					return static_cast<int>(value);
				}
				case SchemaExtensionPropertyType::Number: {
					if (lua_type(L, index) != LUA_TNUMBER) luaL_typeerrorL(L, index, "number");
					const auto value = lua_tonumber(L, index);
					if (!std::isfinite(value)) throw std::invalid_argument("Extension Number value must be finite");
					return static_cast<double>(value);
				}
				case SchemaExtensionPropertyType::String: {
					if (lua_type(L, index) != LUA_TSTRING) luaL_typeerrorL(L, index, "string");
					std::size_t length = 0;
					const auto *value = lua_tolstring(L, index, &length);
					return std::string(value, length);
				}
			}
			throw std::invalid_argument("Unknown extension property type");
		}

		std::any CustomWireValueToAny(const WireValue &value) {
			return std::visit([](const auto &typed) -> std::any {
				using Value = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, int> ||
					std::is_same_v<Value, double> || std::is_same_v<Value, std::string>) return typed;
				else throw std::invalid_argument("Custom class property has an unsupported stored value type");
			}, value);
		}

		WireValue CustomAnyToWireValue(SchemaExtensionPropertyType type, const std::any &value) {
			switch (type) {
				case SchemaExtensionPropertyType::Boolean:
					if (const auto *typed = std::any_cast<bool>(&value)) return *typed;
					break;
				case SchemaExtensionPropertyType::Integer:
					if (const auto *typed = std::any_cast<int>(&value)) return *typed;
					break;
				case SchemaExtensionPropertyType::Number:
					if (const auto *typed = std::any_cast<double>(&value); typed && std::isfinite(*typed)) return *typed;
					break;
				case SchemaExtensionPropertyType::String:
					if (const auto *typed = std::any_cast<std::string>(&value)) return *typed;
					break;
			}
			throw std::invalid_argument("Custom class property value has the wrong native type");
		}

		int PushExtensionWireValue(lua_State *L, const WireValue &value) {
			return std::visit([L](const auto &typed) -> int {
				using Value = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, int> ||
					std::is_same_v<Value, double> || std::is_same_v<Value, std::string>)
					return StackValue<Value>::Push(L, typed);
				else throw std::runtime_error("Stored extension property type is unsupported");
			}, value);
		}

		const SchemaClassDefinition &GetInstanceSchemaClass(const Instance *instance) {
			auto *definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(instance));
			if (!definition) throw std::runtime_error("Instance has no runtime schema class definition");
			return *definition;
		}

		std::size_t ValidateExtensionOverrides(
			const Instance *instance,
			const std::map<SchemaId, std::map<std::string, WireValue>> &overrides
		) {
			const auto &registry = GetActiveRuntimeSchemaRegistry();
			const auto &classDefinition = GetInstanceSchemaClass(instance);
			std::size_t count = 0;
			std::size_t bytes = 0;
			for (const auto &[extensionId, values] : overrides) {
				auto *extension = registry.FindExtensionById(extensionId);
				if (!extension || !registry.IsExtensionApplicableToClass(extensionId, classDefinition.Id))
					throw std::invalid_argument("Extension override does not apply to the Instance class");
				for (const auto &[name, value] : values) {
					auto *property = registry.FindExtensionProperty(extensionId, name);
					if (!property) throw std::invalid_argument("Extension override names an unknown property");
					if (value == property->DefaultValue)
						throw std::invalid_argument("Extension override redundantly stores its schema default");
					++count;
					bytes += sizeof(SchemaId) + sizeof(extension->DefinitionVersion) + name.size() +
						ValidateSchemaExtensionPropertyValue(property->Type, value);
					if (count > MaximumExtensionOverridesPerInstance ||
						bytes > MaximumExtensionOverrideBytesPerInstance)
						throw std::invalid_argument("Instance exceeds its extension override limits");
				}
			}
			return bytes;
		}

		std::size_t ValidateCustomPropertyOverrides(
			const Instance *instance,
			const std::map<SchemaId, std::map<std::string, WireValue>> &overrides
		) {
			const auto &registry = GetActiveRuntimeSchemaRegistry();
			const auto &classDefinition = GetInstanceSchemaClass(instance);
			std::size_t count = 0;
			std::size_t bytes = 0;
			for (const auto &[declaringClassId, values] : overrides) {
				auto *declaringClass = registry.FindClassById(declaringClassId);
				if (!declaringClass || declaringClass->ConstructionKind != SchemaClassConstructionKind::CustomData ||
					!registry.IsClassDerivedFrom(classDefinition.Id, declaringClassId))
					throw std::invalid_argument("Custom property override does not belong to the Instance class");
				for (const auto &[name, value] : values) {
					auto *property = registry.FindCustomClassProperty(declaringClassId, name);
					if (!property) throw std::invalid_argument("Custom property override names an unknown property");
					if (value == property->DefaultValue)
						throw std::invalid_argument("Custom property override redundantly stores its schema default");
					++count;
					bytes += sizeof(SchemaId) + sizeof(declaringClass->DefinitionVersion) + name.size() +
						ValidateSchemaExtensionPropertyValue(property->Type, value);
					if (count > MaximumCustomPropertyOverridesPerInstance ||
						bytes > MaximumCustomPropertyOverrideBytesPerInstance)
						throw std::invalid_argument("Instance exceeds its custom property override limits");
				}
			}
			return bytes;
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

	InstanceProperty MakeCustomClassInstanceProperty(
		SchemaId declaringClassId,
		std::uint32_t definitionVersion,
		const SchemaClassProperty &property
	) {
		InstanceProperty result(property.Name);
		result.ReflectedTypedef = property.Type == SchemaExtensionPropertyType::Boolean ? "boolean" :
			property.Type == SchemaExtensionPropertyType::String ? "string" : "number";
		result.SemanticType = property.Type == SchemaExtensionPropertyType::Boolean ? InstanceProperty::DataType::Bool :
			property.Type == SchemaExtensionPropertyType::Integer ? InstanceProperty::DataType::Integer :
			property.Type == SchemaExtensionPropertyType::Number ? InstanceProperty::DataType::Number :
			InstanceProperty::DataType::String;
		result.WireType = property.Type == SchemaExtensionPropertyType::Boolean ? "Bool" :
			property.Type == SchemaExtensionPropertyType::Integer ? "Int" :
			property.Type == SchemaExtensionPropertyType::Number ? "Double" : "String";
		result.Category = "Data";
		result.Unmodified = CustomWireValueToAny(property.DefaultValue);
		result.PersistencePolicy = InstanceProperty::Persistence::Saved;
		result.ReplicationPolicy = InstanceProperty::Replication::FutureReplicated;
		result.WriteAuthority = InstanceProperty::Authority::Main;
		result.Editable = property.Editable;
		result.DeclaringSchemaId = declaringClassId;
		result.DeclaringDefinitionVersion = definitionVersion;
		result.CustomSchemaPropertyType = static_cast<std::uint8_t>(property.Type);
		result.CustomSchemaDefaultValue = property.DefaultValue;
		const auto name = property.Name;
		result.Read = [declaringClassId, name](Instance *self) {
			return CustomWireValueToAny(self->GetCustomClassPropertyValue(declaringClassId, name));
		};
		result.Write = [](Instance *, std::any) {};
		result.Validate = [type = property.Type](const std::any &value) {
			try {
				(void)ValidateSchemaExtensionPropertyValue(type, CustomAnyToWireValue(type, value));
				return true;
			} catch (const std::invalid_argument &) { return false; }
		};
		result.PushStack = [](lua_State *L, std::any value) {
			if (auto *typed = std::any_cast<bool>(&value)) return StackValue<bool>::Push(L, *typed);
			if (auto *typed = std::any_cast<int>(&value)) return StackValue<int>::Push(L, *typed);
			if (auto *typed = std::any_cast<double>(&value)) return StackValue<double>::Push(L, *typed);
			if (auto *typed = std::any_cast<std::string>(&value)) return StackValue<std::string>::Push(L, *typed);
			throw std::invalid_argument("Custom class property cannot be pushed to Luau");
		};
		result.IsStack = [type = property.Type](lua_State *L, int index) {
			return type == SchemaExtensionPropertyType::Boolean ? lua_type(L, index) == LUA_TBOOLEAN :
				type == SchemaExtensionPropertyType::String ? lua_type(L, index) == LUA_TSTRING :
				lua_type(L, index) == LUA_TNUMBER;
		};
		result.FromStack = [type = property.Type](lua_State *L, int index) {
			return CustomWireValueToAny(ReadExtensionWireValue(L, index, type));
		};
		return result;
	}

	G_USERDATA_IMPL(
		Instance,
		.Tag = UserdataTag::Instance,
		.Type = "Instance",
		.Methods = {
			{"__index", Method{&Instance::LIndex}},
			{"__newindex", Method{&Instance::LNewIndex}},
			{"__namecall", Method{&Instance::LNamecall}},
			{"__eq", Method{&Instance::LEqual}},
		}
	);

	Instance::Instance() { RequireFrozenRuntimeSchema("Instance construction"); }

	void Instance::BindRuntimeSchemaClass(SchemaId classId) {
		if (RuntimeSchemaClassId.IsValid()) throw std::logic_error("Instance runtime schema class is already bound");
		auto *definition = GetActiveRuntimeSchemaRegistry().FindClassById(classId);
		auto *nativeHost = InstanceClassRegistry::GetDefinitionByType(std::type_index(typeid(*this)));
		if (!definition || definition->ConstructionKind != SchemaClassConstructionKind::CustomData ||
			!nativeHost || definition->NativeHostClassId != nativeHost->Id)
			throw std::invalid_argument("Custom class cannot bind to this native host type");
		RuntimeSchemaClassId = classId;
		Name = definition->CanonicalName;
	}

	Instance::~Instance() {
		if (auto DataModelValue = OwningDataModel.lock(); DataModelValue && !Destroyed)
			DataModelValue->ReleaseInstance();
		ObjectRegistry::Get().Invalidate(Id);
	}

	ObjectId Instance::GetObjectId() const {
		std::scoped_lock lock(IdentityMutex);
		if (!Id.IsValid()) {
			AssertAuthoritativeMutation("Instance identity publication");
			auto self = const_cast<Instance *>(this)->shared_from_this();
			Id = ObjectRegistry::Get().Register(self);
			auto definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(this));
			const auto scope = IsNativeDataModelDefinition(definition) ? Id : GetReplicationScopeId();
			if (scope.IsValid()) ChangeJournal::Get().Commit(scope, Id, ObjectCreatedChange{
				definition ? (definition->ConstructionKind == SchemaClassConstructionKind::CustomData
					? definition->CanonicalName : definition->ClassName) : "Instance",
				definition ? definition->Id : SchemaId{},
				definition ? definition->DefinitionVersion : 0,
			});
		}
		return Id;
	}

	void Instance::Destroy() {
		AssertAuthoritativeMutation("Instance::Destroy");
		if (Destroyed || DestroyingState) return;
		if (auto dataModel = GetDataModel()) dataModel->EnsureAuthoritativeRevisionAvailable();
		DestroyingState = true;
		Destroyed = true;
		auto dataModel = GetDataModel();
		const auto objectId = GetObjectId();
		const auto scope = GetReplicationScopeId();
		if (auto dataModel = GetDataModel()) dataModel->Tags.RemoveAll(scope, objectId);
		Attributes.clear();
		ExtensionValues.clear();
		CustomPropertyValues.clear();
		AttributeChangedSignals.clear();
		ObjectRegistry::Get().Invalidate(objectId);
		if (scope.IsValid())
			ChangeJournal::Get().Commit(scope, objectId, PropertyUpdatedChange{"Destroyed", true, true});
		GetPropertyChangedSignal("Destroyed")->Fire({});

		Destroying->Fire({});

		auto children = Children;
		for (auto &child : children) {
			child->Destroy();
		}

		SetParent(nullptr);
		if (auto Definition = InstanceClassRegistry::GetDefinition(this)) {
			for (const auto &[Name, Property] : Definition->AllProperties) {
				(void)Name;
				if (Property && Property->Signal && Property->ReadSignal) {
					if (auto Signal = Property->ReadSignal(this)) Signal->DisconnectAll();
				}
			}
		}
		for (auto &[Name, Signal] : PropertyChangedSignals) {
			(void)Name;
			if (Signal) Signal->DisconnectAll();
		}
		PropertyChangedSignals.clear();
		if (scope.IsValid()) ChangeJournal::Get().Commit(scope, objectId, ObjectDestroyedChange{});
		if (dataModel) dataModel->AdvanceAuthoritativeRevision();
		if (dataModel && dataModel.get() != this) {
			OwningDataModel.reset();
			dataModel->ReleaseInstance();
		}
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
		if (propertyName == "Name" || property->PersistencePolicy == InstanceProperty::Persistence::Saved)
			if (auto dataModel = GetDataModel()) dataModel->AdvanceAuthoritativeRevision();
		GetPropertyChangedSignal(std::string(propertyName))->Fire({});
	}

	ObjectId Instance::GetReplicationScopeId() const {
		auto DataModelValue = GetDataModel();
		return DataModelValue ? DataModelValue->GetObjectId() : ObjectId{};
	}

	std::shared_ptr<DataModel> Instance::GetDataModel() const {
		if (auto Owner = OwningDataModel.lock()) return Owner;
		std::shared_ptr<Instance> root = const_cast<Instance *>(this)->shared_from_this();
		while (auto parent = root->ParentReference.lock()) root = std::move(parent);
		return std::dynamic_pointer_cast<DataModel>(root);
	}

	void Instance::PublishReplicationSubtree(ObjectId scope) {
		if (!scope.IsValid()) return;
		std::vector<std::pair<ObjectId, ChangePayload>> Changes;
		std::function<void(Instance *)> Append = [&](Instance *Current) {
			auto *definition = InstanceClassRegistry::GetDefinition(Current);
			if (!definition) throw std::runtime_error("Replicated object has no class definition");
			const auto objectId = Current->GetObjectId();
			Changes.emplace_back(objectId, ObjectCreatedChange{
				definition->ConstructionKind == SchemaClassConstructionKind::CustomData
					? definition->CanonicalName : definition->ClassName,
				definition->Id,
				definition->DefinitionVersion,
			});
			for (const auto &[name, property] : definition->AllProperties) {
				if (property->CustomSchemaPropertyType) continue;
				if (property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
					!property->Read || !property->Write)
					continue;
				Changes.emplace_back(objectId, PropertyUpdatedChange{
					name,
					EncodeCommittedProperty(Current, *property),
					true,
					property->CustomSchemaPropertyType ? std::optional(property->DeclaringSchemaId) : std::nullopt,
					property->CustomSchemaPropertyType ? property->DeclaringDefinitionVersion : 0,
				});
			}
			for (const auto &[name, value] : Current->Attributes)
				Changes.emplace_back(objectId, AttributeUpdatedChange{name, value});
			for (const auto &[extensionId, values] : Current->ExtensionValues) {
				auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(extensionId);
				if (!extension) throw std::runtime_error("Instance contains state for a missing extension definition");
				for (const auto &[name, value] : values)
					Changes.emplace_back(objectId, ExtensionPropertyUpdatedChange{
						extensionId, extension->DefinitionVersion, name, value
					});
			}
			for (const auto &[declaringClassId, values] : Current->CustomPropertyValues) {
				auto *declaringClass = GetActiveRuntimeSchemaRegistry().FindClassById(declaringClassId);
				if (!declaringClass) throw std::runtime_error("Instance contains state for a missing custom class definition");
				for (const auto &[name, value] : values)
					Changes.emplace_back(objectId, PropertyUpdatedChange{
						name, value, true, declaringClassId, declaringClass->DefinitionVersion
					});
			}
			auto parent = Current->ParentReference.lock();
			Changes.emplace_back(objectId, ObjectReparentedChange{
				parent ? std::optional(parent->GetObjectId()) : std::nullopt
			});
			if (auto dataModel = Current->GetDataModel()) {
				for (const auto &name : dataModel->Tags.GetTags(scope, objectId, ScriptSecurityContext::CoreTrusted()))
					Changes.emplace_back(objectId, TagAddedChange{name});
			}
			for (const auto &child : Current->Children) Append(child.get());
		};
		Append(this);
		ChangeJournal::Get().CommitBatch(scope, std::move(Changes));
	}

	std::optional<WireValue> Instance::ReadPropertyWireValue(std::string_view PropertyName) const {
		auto *Property = const_cast<Instance *>(this)->FindProperty(std::string(PropertyName));
		if (!Property || !Property->Read) return std::nullopt;
		return EncodeCommittedProperty(const_cast<Instance *>(this), *Property);
	}

	void Instance::MarkPersistenceSubtreeArchivable() {
		Archivable = true;
		for (const auto &Child : Children) Child->MarkPersistenceSubtreeArchivable();
	}

	void Instance::SetDetachedTagsForAdoption(std::vector<std::string> Tags) {
		if (GetDataModel()) throw std::invalid_argument("Detached tags can only be assigned before DataModel adoption");
		if (Tags.size() > MaximumTagsPerInstance) throw std::length_error("Detached tag state exceeds its count limit");
		std::sort(Tags.begin(), Tags.end());
		if (std::adjacent_find(Tags.begin(), Tags.end()) != Tags.end())
			throw std::invalid_argument("Detached tag state contains a duplicate tag");
		for (const auto &Tag : Tags) ValidateTagName(Tag);
		DetachedTags = std::move(Tags);
	}

	std::shared_ptr<Instance> Instance::Clone() {
		AssertIsAlive();
		auto SourceRoot = shared_from_this();
		auto SourceDataModel = GetDataModel();
		if (SourceDataModel && SourceDataModel->IsProtectedService(SourceRoot))
			throw std::invalid_argument(std::format("Cannot clone {}: class is a protected service", GetClassName()));

		std::vector<std::shared_ptr<Instance>> SourceNodes;
		std::vector<std::pair<std::shared_ptr<Instance>, std::size_t>> Pending{{SourceRoot, 1}};
		while (!Pending.empty()) {
			auto [Source, Depth] = std::move(Pending.back());
			Pending.pop_back();
			if (Depth > MaximumProtocolJsonDepth)
				throw std::length_error(std::format("Cannot clone {}: subtree exceeds maximum depth", GetClassName()));
			if (SourceNodes.size() == MaximumPersistenceObjects)
				throw std::length_error(std::format("Cannot clone {}: subtree exceeds maximum object count", GetClassName()));
			Source->AssertIsAlive();
			auto *Definition = InstanceClassRegistry::GetDefinition(Source.get());
			if (!Definition || !Definition->EditorVisible || !InstanceClassRegistry::IsConstructible(*Definition))
				throw std::invalid_argument(std::format("Cannot clone {}: class is not cloneable", Source->GetClassName()));
			if (!Source->GetArchivable())
				throw std::invalid_argument(std::format("Cannot clone {} '{}': Archivable is false", Source->GetClassName(), Source->GetName()));
			SourceNodes.push_back(Source);
			for (auto Child = Source->Children.rbegin(); Child != Source->Children.rend(); ++Child)
				Pending.emplace_back(*Child, Depth + 1);
		}

		std::string Encoded = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, SourceRoot
		);
		if (Encoded.size() > MaximumProtocolDocumentBytes)
			throw std::length_error(std::format("Cannot clone {}: semantic state exceeds its byte limit", GetClassName()));
		std::istringstream Input(std::move(Encoded));
		InstanceSerialization::DeserializationState State;
		{
			ScopedChangeJournalSuppression Suppression;
			State = InstanceSerialization::DeserializeDetached(InstanceSerialization::InstanceFormat::Json, Input);
		}
		if (!State.Ok || !State.Instance) {
			const auto Reason = State.Errors.empty() ? std::string("persistent state could not be reconstructed") : State.Errors.front();
			throw std::invalid_argument(std::format("Cannot clone {}: {}", GetClassName(), Reason));
		}

		auto CloneRoot = State.Instance;
		std::vector<std::shared_ptr<Instance>> CloneNodes{CloneRoot};
		CloneRoot->CollectDescendants(CloneNodes);
		if (CloneNodes.size() != SourceNodes.size()) {
			CloneRoot->Destroy();
			throw std::runtime_error(std::format("Cannot clone {}: reconstructed subtree is inconsistent", GetClassName()));
		}
		std::unordered_map<const Instance *, std::shared_ptr<Instance>> Remapped;
		Remapped.reserve(SourceNodes.size());
		for (std::size_t Index = 0; Index < SourceNodes.size(); ++Index)
			Remapped.emplace(SourceNodes[Index].get(), CloneNodes[Index]);

		try {
			ScopedChangeJournalSuppression Suppression;
			for (std::size_t Index = 0; Index < SourceNodes.size(); ++Index) {
				auto &Source = SourceNodes[Index];
				auto &Copy = CloneNodes[Index];
				auto *Definition = InstanceClassRegistry::GetDefinition(Source.get());
				std::vector<std::string> PropertyNames;
				PropertyNames.reserve(Definition->AllProperties.size());
				for (const auto &[Name, Property] : Definition->AllProperties)
					if (Name != "Parent" && Name != "Name" && Name != "SourceVersion" &&
						!Property->Signal && Property->Read && Property->Write &&
						Property->WritePermission != Enums::Permission::Never)
						PropertyNames.push_back(Name);
				std::sort(PropertyNames.begin(), PropertyNames.end());
				for (const auto &Name : PropertyNames) {
					auto *Property = Definition->AllProperties.at(Name);
					if (Property->ReadObjectReference && Property->WriteObjectReference) {
						auto Referenced = Property->ReadObjectReference(Source.get());
						if (Referenced) if (auto Found = Remapped.find(Referenced.get()); Found != Remapped.end())
							Referenced = Found->second;
						Property->WriteObjectReference(Copy.get(), std::move(Referenced));
					} else if (Property->PersistencePolicy != InstanceProperty::Persistence::Saved) {
						const auto Status = Copy->ApplyPropertyMutation(
							Name, Property->Read(Source.get()), Enums::Permission::Engine,
							ScriptSecurityContext::CoreTrusted()
						);
						if (Status != MutationStatus::Success)
							throw std::invalid_argument(std::format("property {} could not be copied", Name));
					}
				}
			}
		} catch (const std::exception &Error) {
			auto Reason = std::string(Error.what());
			if (Reason.size() > 512) Reason = Reason.substr(0, 499) + "...<truncated>";
			CloneRoot->Destroy();
			throw std::invalid_argument(std::format("Cannot clone {}: {}", GetClassName(), Reason));
		} catch (...) {
			CloneRoot->Destroy();
			throw std::invalid_argument(std::format("Cannot clone {}: semantic state copy failed", GetClassName()));
		}
		return CloneRoot;
	}

	void Instance::AssertCanMutate() const {
		AssertAuthoritativeMutation("Instance property mutation");
		AssertIsAlive();
	}

	void Instance::ValidatePropertyMutation(std::string_view propertyName, const std::any &value) const {
		auto *property = const_cast<Instance *>(this)->FindProperty(std::string(propertyName));
		if (!property) throw std::invalid_argument("Property does not exist");
		if (!property->IsValueValid(value)) throw std::invalid_argument("Property validation failed");
		if (propertyName == "Name" || property->PersistencePolicy == InstanceProperty::Persistence::Saved)
			if (auto dataModel = GetDataModel()) dataModel->EnsureAuthoritativeRevisionAvailable();
	}

	MutationStatus Instance::ApplyPropertyMutation(
		std::string_view propertyName,
		const std::any &value,
		Enums::Permission permission,
		const ScriptSecurityContext &securityContext
	) {
		if (Destroyed || DestroyingState) return MutationStatus::StaleObject;
		auto *property = FindProperty(std::string(propertyName));
		if (!property) return MutationStatus::InvalidProperty;
		if (!property->Write || property->WritePermission == Enums::Permission::Never) return MutationStatus::ReadOnly;
		if (!property->CanWrite(securityContext)) return MutationStatus::Unauthorized;
		if (static_cast<int>(permission) < static_cast<int>(property->WritePermission)) return MutationStatus::Unauthorized;
		if (property->WriteAuthority == InstanceProperty::Authority::Main &&
			GetCurrentExecutionDomain() != ExecutionDomain::Main)
			return MutationStatus::WrongExecutionDomain;
		if (!property->IsValueValid(value)) return MutationStatus::ValidationFailed;
		if (property->WriteObjectReference) {
			if (!property->DecodeObjectReference) return MutationStatus::ValidationFailed;
			try {
				auto Referenced = property->DecodeObjectReference(value);
				if (Referenced) {
					if (Referenced->GetDestroyed() || Referenced->IsDestroying()) return MutationStatus::StaleObject;
					if (propertyName != "Parent" && Referenced->GetReplicationScopeId() != GetReplicationScopeId())
						return MutationStatus::ValidationFailed;
				}
			} catch (const std::exception &) {
				return MutationStatus::ValidationFailed;
			}
		}
		if (property->CustomSchemaPropertyType) {
			return ApplyCustomClassPropertyMutation(
				property->DeclaringSchemaId,
				property->DeclaringDefinitionVersion,
				propertyName,
				CustomAnyToWireValue(
					static_cast<SchemaExtensionPropertyType>(*property->CustomSchemaPropertyType), value
				),
				securityContext
			);
		}
		if (property->Read) {
			auto current = EncodeNativeWireValue(property->Read(this));
			auto requested = EncodeNativeWireValue(value);
			if (current && requested && *current == *requested) return MutationStatus::Success;
		}
		property->Write(this, value);
		return MutationStatus::Success;
	}

	MutationStatus Instance::ApplyPropertyWireMutation(
		std::string_view PropertyName,
		const WireValue &Value,
		Enums::Permission Permission,
		const ScriptSecurityContext &SecurityContext
	) {
		if (Destroyed || DestroyingState) return MutationStatus::StaleObject;
		auto *Property = FindProperty(std::string(PropertyName));
		if (!Property) return MutationStatus::InvalidProperty;
		if (!Property->Write || Property->WritePermission == Enums::Permission::Never) return MutationStatus::ReadOnly;
		if (!Property->CanWrite(SecurityContext)) return MutationStatus::Unauthorized;
		if (static_cast<int>(Permission) < static_cast<int>(Property->WritePermission)) return MutationStatus::Unauthorized;
		if (Property->WriteAuthority == InstanceProperty::Authority::Main &&
			GetCurrentExecutionDomain() != ExecutionDomain::Main)
			return MutationStatus::WrongExecutionDomain;

		if (const auto *EnumValue = std::get_if<WireEnumItem>(&Value)) {
			if (Property->SemanticType != InstanceProperty::DataType::NativeEnum ||
				!Property->NativeEnumType || *Property->NativeEnumType != EnumValue->EnumType ||
				!Property->WriteEnumValue || !Property->ReadEnumValue)
				return MutationStatus::ValidationFailed;
			auto EnumType = Enums::GetEnums().find(*Property->NativeEnumType);
			if (EnumType == Enums::GetEnums().end()) return MutationStatus::ValidationFailed;
			auto Item = EnumType->second->FromName(EnumValue->Item);
			if (!Item) return MutationStatus::ValidationFailed;
			auto [CurrentType, CurrentValue] = Property->ReadEnumValue(this);
			if (CurrentType == EnumValue->EnumType && CurrentValue == Item->Value) return MutationStatus::Success;
			Property->WriteEnumValue(this, Item->Value);
			return MutationStatus::Success;
		}

		const auto *Reference = std::get_if<WireObjectReference>(&Value);
		if (std::holds_alternative<std::monostate>(Value) || Reference) {
			if (Property->SemanticType != InstanceProperty::DataType::ObjectReference ||
				!Property->WriteObjectReference || !Property->ObjectReferenceClassSchemaId)
				return MutationStatus::ValidationFailed;
			std::shared_ptr<Instance> Referenced;
			if (Reference) {
				Referenced = ObjectRegistry::Get().Lookup(Reference->Object.ToObjectId());
				if (!Referenced || Referenced->GetDestroyed() || Referenced->IsDestroying())
					return MutationStatus::StaleObject;
				auto *ReferencedClass = InstanceClassRegistry::GetDefinition(Referenced.get());
				if (!ReferencedClass || !GetActiveRuntimeSchemaRegistry().IsClassDerivedFrom(
						ReferencedClass->Id, *Property->ObjectReferenceClassSchemaId
					)) return MutationStatus::ValidationFailed;
				if (PropertyName != "Parent" && Referenced->GetReplicationScopeId() != GetReplicationScopeId())
					return MutationStatus::ValidationFailed;
			} else if (!Property->Nullable) return MutationStatus::ValidationFailed;
			if (Property->ReadObjectReference(this) == Referenced) return MutationStatus::Success;
			Property->WriteObjectReference(this, Referenced);
			return MutationStatus::Success;
		}

		if (Property->SemanticType == InstanceProperty::DataType::NativeEnum ||
			Property->SemanticType == InstanceProperty::DataType::SchemaEnum ||
			Property->SemanticType == InstanceProperty::DataType::ObjectReference)
			return MutationStatus::ValidationFailed;
		auto Native = DecodeNativeWireValue(Value);
		if (!Native) return MutationStatus::ValidationFailed;
		return ApplyPropertyMutation(PropertyName, *Native, Permission, SecurityContext);
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
		if (auto found = AttributeChangedSignals.find(std::string(name)); found != AttributeChangedSignals.end())
			return found->second;
		if (AttributeChangedSignals.size() >= MaximumAttributeSignalsPerInstance)
			throw std::invalid_argument("Instance exceeds its attribute signal count limit");
		auto signal = std::make_shared<Signal<std::monostate>>();
		AttributeChangedSignals.emplace(std::string(name), signal);
		return signal;
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
		if (auto dataModel = GetDataModel()) dataModel->EnsureAuthoritativeRevisionAvailable();
		Attributes.swap(candidate);
		try {
			ChangeJournal::Get().Commit(GetReplicationScopeId(), GetObjectId(), AttributeUpdatedChange{
				std::string(name), value
			});
		} catch (...) {
			Attributes.swap(candidate);
			throw;
		}
		if (auto dataModel = GetDataModel()) dataModel->AdvanceAuthoritativeRevision();
		auto signal = AttributeChangedSignals.find(std::string(name));
		if (signal != AttributeChangedSignals.end()) signal->second->Fire({});
		return MutationStatus::Success;
	}

	WireValue Instance::GetExtensionPropertyValue(
		SchemaId extensionId,
		std::string_view propertyName,
		const ScriptSecurityContext &securityContext
	) const {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Extension property read requires ReadDataModel");
		AssertIsAlive();
		const auto &registry = GetActiveRuntimeSchemaRegistry();
		const auto &classDefinition = GetInstanceSchemaClass(this);
		if (!registry.IsExtensionApplicableToClass(extensionId, classDefinition.Id))
			throw std::invalid_argument("Extension does not apply to the Instance class");
		auto *property = registry.FindExtensionProperty(extensionId, propertyName);
		if (!property) throw std::invalid_argument("Extension property does not exist");
		auto extension = ExtensionValues.find(extensionId);
		if (extension != ExtensionValues.end()) {
			auto value = extension->second.find(std::string(propertyName));
			if (value != extension->second.end()) return value->second;
		}
		return property->DefaultValue;
	}

	std::map<SchemaId, std::map<std::string, WireValue>> Instance::GetExtensionPropertyOverrides(
		const ScriptSecurityContext &securityContext
	) const {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Extension property read requires ReadDataModel");
		AssertIsAlive();
		(void)ValidateExtensionOverrides(this, ExtensionValues);
		return ExtensionValues;
	}

	MutationStatus Instance::ApplyExtensionPropertyMutation(
		SchemaId extensionId,
		std::uint32_t definitionVersion,
		std::string_view propertyName,
		WireValue value,
		const ScriptSecurityContext &securityContext
	) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return MutationStatus::WrongExecutionDomain;
		if (!securityContext.HasCapability(ScriptCapability::MutateDataModel)) return MutationStatus::Unauthorized;
		AssertIsAlive();
		const auto &registry = GetActiveRuntimeSchemaRegistry();
		auto *extension = registry.FindExtensionById(extensionId);
		if (!extension || extension->DefinitionVersion != definitionVersion) return MutationStatus::InvalidProperty;
		const auto &classDefinition = GetInstanceSchemaClass(this);
		if (!registry.IsExtensionApplicableToClass(extensionId, classDefinition.Id))
			return MutationStatus::InvalidProperty;
		auto *property = registry.FindExtensionProperty(extensionId, propertyName);
		if (!property) return MutationStatus::InvalidProperty;
		try { (void)ValidateSchemaExtensionPropertyValue(property->Type, value); }
		catch (const std::invalid_argument &) { return MutationStatus::ValidationFailed; }

		auto current = property->DefaultValue;
		if (auto extensionValues = ExtensionValues.find(extensionId); extensionValues != ExtensionValues.end()) {
			if (auto found = extensionValues->second.find(std::string(propertyName)); found != extensionValues->second.end())
				current = found->second;
		}
		if (current == value) return MutationStatus::Success;

		auto candidate = ExtensionValues;
		if (value == property->DefaultValue) {
			auto found = candidate.find(extensionId);
			if (found != candidate.end()) {
				found->second.erase(std::string(propertyName));
				if (found->second.empty()) candidate.erase(found);
			}
		} else {
			candidate[extensionId][std::string(propertyName)] = value;
		}
		(void)ValidateExtensionOverrides(this, candidate);
		if (auto dataModel = GetDataModel()) dataModel->EnsureAuthoritativeRevisionAvailable();
		ExtensionValues.swap(candidate);
		try {
			ChangeJournal::Get().Commit(GetReplicationScopeId(), GetObjectId(), ExtensionPropertyUpdatedChange{
				extensionId, definitionVersion, std::string(propertyName), std::move(value)
			});
		} catch (...) {
			ExtensionValues.swap(candidate);
			throw;
		}
		if (auto dataModel = GetDataModel()) dataModel->AdvanceAuthoritativeRevision();
		return MutationStatus::Success;
	}

	WireValue Instance::GetCustomClassPropertyValue(
		SchemaId declaringClassId,
		std::string_view propertyName,
		const ScriptSecurityContext &securityContext
	) const {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Custom class property read requires ReadDataModel");
		AssertIsAlive();
		const auto &registry = GetActiveRuntimeSchemaRegistry();
		const auto &classDefinition = GetInstanceSchemaClass(this);
		if (!registry.IsClassDerivedFrom(classDefinition.Id, declaringClassId))
			throw std::invalid_argument("Custom property does not apply to the Instance class");
		auto *property = registry.FindCustomClassProperty(declaringClassId, propertyName);
		if (!property) throw std::invalid_argument("Custom class property does not exist");
		if (auto owner = CustomPropertyValues.find(declaringClassId); owner != CustomPropertyValues.end())
			if (auto value = owner->second.find(std::string(propertyName)); value != owner->second.end())
				return value->second;
		return property->DefaultValue;
	}

	std::map<SchemaId, std::map<std::string, WireValue>> Instance::GetCustomClassPropertyOverrides(
		const ScriptSecurityContext &securityContext
	) const {
		if (!securityContext.HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Custom class property read requires ReadDataModel");
		AssertIsAlive();
		(void)ValidateCustomPropertyOverrides(this, CustomPropertyValues);
		return CustomPropertyValues;
	}

	MutationStatus Instance::ApplyCustomClassPropertyMutation(
		SchemaId declaringClassId,
		std::uint32_t definitionVersion,
		std::string_view propertyName,
		WireValue value,
		const ScriptSecurityContext &securityContext
	) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return MutationStatus::WrongExecutionDomain;
		if (!securityContext.HasCapability(ScriptCapability::MutateDataModel)) return MutationStatus::Unauthorized;
		AssertIsAlive();
		const auto &registry = GetActiveRuntimeSchemaRegistry();
		auto *declaringClass = registry.FindClassById(declaringClassId);
		if (!declaringClass || declaringClass->ConstructionKind != SchemaClassConstructionKind::CustomData ||
			declaringClass->DefinitionVersion != definitionVersion) return MutationStatus::InvalidProperty;
		const auto &classDefinition = GetInstanceSchemaClass(this);
		if (!registry.IsClassDerivedFrom(classDefinition.Id, declaringClassId)) return MutationStatus::InvalidProperty;
		auto *property = registry.FindCustomClassProperty(declaringClassId, propertyName);
		if (!property) return MutationStatus::InvalidProperty;
		try { (void)ValidateSchemaExtensionPropertyValue(property->Type, value); }
		catch (const std::invalid_argument &) { return MutationStatus::ValidationFailed; }

		auto current = property->DefaultValue;
		if (auto owner = CustomPropertyValues.find(declaringClassId); owner != CustomPropertyValues.end())
			if (auto found = owner->second.find(std::string(propertyName)); found != owner->second.end()) current = found->second;
		if (current == value) return MutationStatus::Success;
		auto candidate = CustomPropertyValues;
		if (value == property->DefaultValue) {
			if (auto owner = candidate.find(declaringClassId); owner != candidate.end()) {
				owner->second.erase(std::string(propertyName));
				if (owner->second.empty()) candidate.erase(owner);
			}
		} else candidate[declaringClassId][std::string(propertyName)] = value;
		(void)ValidateCustomPropertyOverrides(this, candidate);
		if (auto dataModel = GetDataModel()) dataModel->EnsureAuthoritativeRevisionAvailable();
		CustomPropertyValues.swap(candidate);
		try {
			ChangeJournal::Get().Commit(GetReplicationScopeId(), GetObjectId(), PropertyUpdatedChange{
				std::string(propertyName), value, true, declaringClassId, definitionVersion
			});
		} catch (...) {
			CustomPropertyValues.swap(candidate);
			throw;
		}
		if (auto dataModel = GetDataModel()) dataModel->AdvanceAuthoritativeRevision();
		GetPropertyChangedSignal(std::string(propertyName))->Fire({});
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

	int Instance::GetExtensionProperty(lua_State *L, Instance *instance) {
		std::size_t extensionLength = 0;
		const auto *extensionName = luaL_checklstring(L, 2, &extensionLength);
		std::size_t propertyLength = 0;
		const auto *propertyName = luaL_checklstring(L, 3, &propertyLength);
		auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionByName(
			std::string_view(extensionName, extensionLength)
		);
		if (!extension) throw std::invalid_argument("Extension definition does not exist or is ambiguous");
		return PushExtensionWireValue(L, instance->GetExtensionPropertyValue(
			extension->Id, std::string_view(propertyName, propertyLength)
		));
	}

	int Instance::SetExtensionProperty(lua_State *L, Instance *instance) {
		std::size_t extensionLength = 0;
		const auto *extensionName = luaL_checklstring(L, 2, &extensionLength);
		std::size_t propertyLength = 0;
		const auto *propertyName = luaL_checklstring(L, 3, &propertyLength);
		auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionByName(
			std::string_view(extensionName, extensionLength)
		);
		if (!extension) throw std::invalid_argument("Extension definition does not exist or is ambiguous");
		auto *property = GetActiveRuntimeSchemaRegistry().FindExtensionProperty(
			extension->Id, std::string_view(propertyName, propertyLength)
		);
		if (!property) throw std::invalid_argument("Extension property does not exist");
		MutationGateway gateway;
		auto result = gateway.Apply(UpdateExtensionPropertyCommand{
			instance->GetObjectId(),
			extension->Id,
			extension->DefinitionVersion,
			std::string(propertyName, propertyLength),
			ReadExtensionWireValue(L, 4, property->Type),
		});
		if (!result.Succeeded())
			throw std::runtime_error(result.Message.empty() ? "Extension property mutation rejected" : result.Message);
		return 0;
	}

	std::string Instance::GetClassName() const {
		AssertIsAlive();
		const InstanceClassDefinition *definition = InstanceClassRegistry::GetDefinition(const_cast<Instance *>(this));
		return definition->ConstructionKind == SchemaClassConstructionKind::CustomData
			? definition->CanonicalName : definition->ClassName;
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
		auto oldDataModel = GetDataModel();
		auto newDataModel = newParent ? newParent->GetDataModel() : oldDataModel;
		if (oldDataModel && newParent && !newDataModel)
			throw std::invalid_argument("Cannot parent a DataModel-owned Instance beneath a detached Instance");
		if (oldDataModel && newParent && newDataModel != oldDataModel)
			throw std::invalid_argument(std::format(
				"Cannot parent {} '{}' to {} '{}': target belongs to a different DataModel",
				GetClassName(), Name, newParent->GetClassName(), newParent->GetName()
			));
		if (!DestroyingState) {
			if (oldDataModel) oldDataModel->EnsureAuthoritativeRevisionAvailable();
			if (newDataModel && newDataModel != oldDataModel) newDataModel->EnsureAuthoritativeRevisionAvailable();
		}

		std::shared_ptr<Instance> self = shared_from_this();
		if (newParent == self) throw std::invalid_argument("An Instance cannot be parented to itself");
		for (auto ancestor = newParent; ancestor; ancestor = ancestor->ParentReference.lock()) {
			if (ancestor == self) throw std::invalid_argument("An Instance cannot be parented to its descendant");
		}
		std::vector<std::shared_ptr<Instance>> subtree;
		subtree.reserve(Children.size() + 1);
		std::vector<std::pair<std::shared_ptr<Instance>, std::size_t>> pending = {{self, 1}};
		std::size_t SubtreeDepth = 0;
		while (!pending.empty()) {
			auto [Node, Depth] = std::move(pending.back());
			pending.pop_back();
			SubtreeDepth = std::max(SubtreeDepth, Depth);
			subtree.push_back(Node);
			if (subtree.size() > MaximumPersistenceObjects)
				throw std::length_error("Cannot set Parent: Instance subtree object-count limit would be exceeded");
			if (Node->Children.size() > MaximumPersistenceObjects - subtree.size() - pending.size())
				throw std::length_error("Cannot set Parent: Instance subtree object-count limit would be exceeded");
			for (const auto &Child : Node->Children) pending.emplace_back(Child, Depth + 1);
		}
		const auto oldScope = oldDataModel ? oldDataModel->GetObjectId() : ObjectId{};
		bool FirstAdoption = !oldDataModel && newDataModel;
		if (FirstAdoption) {
			std::size_t ParentDepth = 0;
			for (auto Current = newParent; Current; Current = Current->ParentReference.lock()) ++ParentDepth;
			if (ParentDepth + SubtreeDepth > MaximumProtocolJsonDepth)
				throw std::length_error("Cannot set Parent: hierarchy depth limit would be exceeded");
			if (!newDataModel->CanAdoptInstances(subtree.size()))
				throw std::length_error("Cannot set Parent: DataModel object-count limit would be exceeded");
			std::unordered_set<const Instance *> SubtreeNodes;
			SubtreeNodes.reserve(subtree.size());
			for (const auto &Node : subtree) SubtreeNodes.insert(Node.get());
			std::set<std::string, std::less<>> NewTagNames;
			for (const auto &Node : subtree) {
				if (Node->GetDestroyed() || Node->IsDestroying())
					throw std::invalid_argument("Cannot adopt a destroyed Instance subtree");
				if (Node->GetDataModel())
					throw std::invalid_argument("Cannot adopt a subtree containing a DataModel-owned Instance");
				auto *Definition = InstanceClassRegistry::GetDefinition(Node.get());
				if (!Definition)
					throw std::invalid_argument("Cannot adopt an Instance with missing schema metadata");
				for (const auto &[PropertyName, Property] : Definition->AllProperties) {
					if (PropertyName == "Parent" || !Property->ReadObjectReference ||
						(Property->PersistencePolicy != InstanceProperty::Persistence::Saved &&
							Property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated)) continue;
					auto Referenced = Property->ReadObjectReference(Node.get());
					if (!Referenced) continue;
					if (Referenced->GetDestroyed() || Referenced->IsDestroying())
						throw std::invalid_argument("Cannot adopt a subtree containing a stale object reference");
					auto ReferencedDataModel = Referenced->GetDataModel();
					if ((ReferencedDataModel && ReferencedDataModel != newDataModel) ||
						(!ReferencedDataModel && !SubtreeNodes.contains(Referenced.get())))
						throw std::invalid_argument(std::format(
							"Cannot adopt {}: object-reference property {} targets an Instance outside the target DataModel",
							InstanceClassName(Node.get()), PropertyName
						));
				}
				if (Node->DetachedTags.size() > MaximumTagsPerInstance)
					throw std::length_error("Cannot adopt an Instance with excessive detached tag state");
				for (const auto &Tag : Node->DetachedTags) {
					ValidateTagName(Tag);
					if (newDataModel->Tags.Find(Tag) == InvalidTagId) NewTagNames.insert(Tag);
				}
				(void)Node->GetObjectId();
			}
			if (NewTagNames.size() > MaximumDistinctTagsPerDataModel - newDataModel->Tags.NameToId.size())
				throw std::length_error("Cannot adopt Instance subtree: DataModel distinct tag limit would be exceeded");
			std::vector<std::pair<ObjectId, std::vector<std::string>>> TagMemberships;
			TagMemberships.reserve(subtree.size());
			for (const auto &Node : subtree) if (!Node->DetachedTags.empty())
				TagMemberships.emplace_back(Node->GetObjectId(), Node->DetachedTags);
			newParent->Children.reserve(newParent->Children.size() + 1);
			newDataModel->AdoptInstances(subtree.size());
			for (const auto &Node : subtree) Node->OwningDataModel = newDataModel;
			try { newDataModel->Tags.AdoptDetached(newDataModel->GetObjectId(), TagMemberships); }
			catch (...) {
				for (const auto &Node : subtree) Node->OwningDataModel.reset();
				for (std::size_t Index = 0; Index < subtree.size(); ++Index) newDataModel->ReleaseInstance();
				throw;
			}
			for (const auto &Node : subtree) Node->DetachedTags.clear();
		}
		const auto newScope = newDataModel ? newDataModel->GetObjectId() : ObjectId{};
		const auto objectId = (oldScope.IsValid() || newScope.IsValid()) ? GetObjectId() : ObjectId{};
		if (!DestroyingState && oldScope != newScope) {
			if (oldDataModel) {
				for (const auto &node : subtree) oldDataModel->Tags.RemoveAll(oldScope, node->GetObjectId());
			}
		}

		// This whole subtree leaves the old ancestry and joins the new one, so
		// collect it once up front and reuse it for both sets of signals

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
			if (oldScope == newScope && newScope.IsValid()) {
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
		if (!DestroyingState) {
			if (oldDataModel) oldDataModel->AdvanceAuthoritativeRevision();
			if (newDataModel && newDataModel != oldDataModel) newDataModel->AdvanceAuthoritativeRevision();
		}
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
			if (self && self->GetDestroyed() && std::string_view(key) != "Destroyed")
				luaL_error(L, "Cannot read %s.%s on destroyed Instance '%s'",
					InstanceClassName(self).c_str(), key, self->Name.c_str());

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
				} else if (auto Service = ResolveServiceInstance(self, key)) {
					StackValue<std::shared_ptr<Instance>>::Push(L, std::move(Service));
					return 1;
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
			const auto ClassName = InstanceClassName(self);
			if (self && self->GetDestroyed())
				luaL_error(L, "Cannot set %s.%s on destroyed Instance '%s'", ClassName.c_str(), key, self->Name.c_str());

			if (key && self) {
				const auto *property = self->FindProperty(key);
				if (property) {
					if (property->Write && property->WritePermission != Enums::Permission::Never) {
						if (!property->CanWrite(GetCurrentScriptSecurityContext()))
							luaL_error(L, "Current script context cannot write property %s", key);
						if (!property->IsStack(L, 3))
							luaL_error(L, "Cannot set %s.%s: expected %s, got %s",
								ClassName.c_str(), key, property->ReflectedTypedef.c_str(), luaL_typename(L, 3));
						auto value = property->FromStack(L, 3);
						const auto status = self->ApplyPropertyMutation(key, value);
						if (status != MutationStatus::Success)
							luaL_error(L, "Cannot set %s.%s on '%s': %s",
								ClassName.c_str(), key, self->Name.c_str(), GetMutationStatusDescription(status));
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
			if (self && self->GetDestroyed() && std::string_view(key) != "Destroy")
				luaL_error(L, "Cannot call %s:%s() on destroyed Instance '%s'",
					InstanceClassName(self).c_str(), key, self->Name.c_str());

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

	int Instance::LEqual(lua_State *L, Instance *self) {
		return InvokeNativeCallback(L, [L, self] {
			auto Other = StackValue<std::shared_ptr<Instance>>::From(L, 2);
			return StackValue<bool>::Push(L, self && Other && self->GetObjectId() == Other->GetObjectId());
		});
	}

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
		auto *requested = InstanceClassRegistry::GetDefinitionByName(className);
		auto *actual = InstanceClassRegistry::GetDefinition(this);
		return requested && actual && actual->InheritedClassIds.contains(requested->Id);
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
		auto *requested = InstanceClassRegistry::GetDefinitionByName(className);
		if (!requested) return nullptr;
		for (const auto &child : Children) {
			if (InstanceClassRegistry::GetDefinition(child.get())->Id == requested->Id) return child;
			if (recursive) {
				if (auto found = child->FindFirstChildOfClass(className, recursive)) return found;
			}
		};
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstChildWhichIsA(std::string className, std::optional<bool> recursive) {
		for (const auto &child : Children) {
			if (child->IsA(className)) return child;
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
		auto *requested = InstanceClassRegistry::GetDefinitionByName(className);
		if (!requested) return nullptr;
		auto current = ParentReference.lock();
		while (current) {
			if (InstanceClassRegistry::GetDefinition(current.get())->Id == requested->Id) {
				return current;
			}
			current = current->ParentReference.lock();
		}
		return nullptr;
	}

	std::shared_ptr<Instance> Instance::FindFirstAncestorWhichIsA(std::string className) {
		auto current = ParentReference.lock();
		while (current) {
			if (current->IsA(className)) {
				return current;
			}
			current = current->ParentReference.lock();
		}
		return nullptr;
	}
}
