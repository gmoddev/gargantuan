#pragma once

#include "gargantuan/reflection/Enums.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/runtime/WireValue.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"
#include <any>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gargantuan {
	#ifdef __NDEBUG__
		#define DEBUG_ENUM_ENTRY Debug = 0
	#else
		#define DEBUG_ENUM_ENTRY Debug
	#endif

	G_ENUM(
		Permission,

		// Minimum of any script
		None,
		// Minimum of any plugin
		Plugin,
		// Minimum of the local development machine, unused in a shipped game
		LocalUser,

		// We skip over WritePlayer since it is irrelevant to Gargantuan;
		// games itself make player instances

		// Minimum of engine-level scripts
		EngineScript = 4,
		// Minimum of Gargantuan itself
		Engine,

		DEBUG_ENUM_ENTRY,

		// This feature should never be used
		Never = 7
	);

	#undef DEBUG_ENUM_ENTRY

	class Instance;
	class BaseSignal;

	class InstanceProperty {
	  private:
		template <typename T> struct MemberPointerTraits;

		template <typename Class, typename T> struct MemberPointerTraits<T Class::*> {
			using ClassType = Class;
			using MemberType = T;
			using ArgsType = std::tuple<>;
		};

		template <typename Class, typename Return, typename... Args>
		struct MemberPointerTraits<Return (Class::*)(Args...)> {
			using ClassType = Class;
			using ReturnType = Return;
			using MemberType = Return;
			using ArgsType = std::tuple<Args...>;
		};

		template <typename T> struct SharedInstancePointer : std::false_type {};
		template <typename T>
		struct SharedInstancePointer<std::shared_ptr<T>> : std::bool_constant<std::is_base_of_v<Instance, T>> {
			using Element = T;
		};
		template <typename T> struct OptionalSharedInstancePointer : std::false_type {};
		template <typename T>
		struct OptionalSharedInstancePointer<std::optional<std::shared_ptr<T>>>
			: std::bool_constant<std::is_base_of_v<Instance, T>> {
			using Element = T;
		};
		template <typename T> struct SharedSignalPointer : std::false_type {};
		template <typename T>
		struct SharedSignalPointer<std::shared_ptr<T>> : std::bool_constant<std::is_base_of_v<BaseSignal, T>> {};

		template <typename Class, typename Return, typename... Args>
		struct MemberPointerTraits<Return (Class::*)(Args...) const> {
			using ClassType = Class;
			using ReturnType = Return;
			using MemberType = Return;
			using ArgsType = std::tuple<Args...>;
		};

	  public:
		enum class Persistence { Transient, Saved };
		enum class Replication { None, FutureReplicated };
		enum class MaterializationDependency { Soft, Hard };
		enum class Authority { Main, Any };
		enum class DataType {
			Unsupported,
			Bool,
			Integer,
			Number,
			String,
			Vector2,
			Vector3,
			Color3,
			UDim,
			UDim2,
			CFrame,
			NativeEnum,
			SchemaEnum,
			ObjectReference,
		};
		struct NumericRange {
			std::optional<double> Minimum;
			std::optional<double> Maximum;
			auto operator<=>(const NumericRange &) const = default;
		};

		std::string Name{};
		std::string ReflectedTypedef{};
		DataType SemanticType = DataType::Unsupported;
		std::string WireType{};
		std::string Category = "Data";
		std::optional<NumericRange> Range{};
		std::optional<std::string> EditorHint{};
		std::optional<std::string> CompoundType{};
		std::optional<std::string> NativeEnumType{};
		std::optional<SchemaId> EnumSchemaId{};
		std::uint32_t EnumDefinitionVersion = 0;
		std::optional<SchemaId> ObjectReferenceClassSchemaId{};
		bool Nullable = false;
		std::any Unmodified{};
		bool Signal{false};
		Persistence PersistencePolicy = Persistence::Transient;
		Replication ReplicationPolicy = Replication::None;
		MaterializationDependency MaterializationDependencyPolicy = MaterializationDependency::Soft;
		Authority WriteAuthority = Authority::Main;
		bool Editable = true;
		ScriptDomainSet ReadDomains = ScriptDomainSet::All();
		ScriptDomainSet WriteDomains = ScriptDomainSet::All();
		ScriptCapability RequiredReadCapability = ScriptCapability::ReadDataModel;
		ScriptCapability RequiredWriteCapability = ScriptCapability::MutateDataModel;
		std::function<bool(const std::any &)> Validate;
		SchemaId DeclaringSchemaId{};
		std::uint32_t DeclaringDefinitionVersion = 0;
		std::optional<std::uint8_t> CustomSchemaPropertyType{};
		WireValue CustomSchemaDefaultValue = std::monostate{};

		Enums::Permission ReadPermission = Enums::Permission::None;
		std::function<std::any(Instance *self)> Read;
		std::function<std::shared_ptr<Instance>(Instance *self)> ReadObjectReference;
		std::function<std::shared_ptr<BaseSignal>(Instance *self)> ReadSignal;
		std::function<std::pair<std::string, int>(Instance *self)> ReadEnumValue;
		std::function<std::optional<int>(const std::any &value)> ReadEncodedEnumValue;
		std::function<int(lua_State *L, std::any value)> PushStack;

		Enums::Permission WritePermission = Enums::Permission::None;
		std::function<void(Instance *self, std::any value)> Write;
		std::function<void(Instance *self, std::shared_ptr<Instance> value)> WriteObjectReference;
		std::function<std::shared_ptr<Instance>(const std::any &value)> DecodeObjectReference;
		std::function<void(Instance *self, int value)> WriteEnumValue;
		std::function<bool(lua_State *L, int idx)> IsStack;
		std::function<std::any(lua_State *L, int idx)> FromStack;

		explicit InstanceProperty(std::string name) : Name(std::move(name)) {};

		InstanceProperty &SetReflectedTypedef(std::string type) {
			ReflectedTypedef = type;
			return *this;
		}

		InstanceProperty &SetDataType(DataType type, std::string wireType) {
			SemanticType = type;
			WireType = std::move(wireType);
			switch (type) {
				case DataType::Vector2: CompoundType = "Vector2"; break;
				case DataType::Vector3: CompoundType = "Vector3"; break;
				case DataType::Color3: CompoundType = "Color3"; break;
				case DataType::UDim: CompoundType = "UDim"; break;
				case DataType::UDim2: CompoundType = "UDim2"; break;
				case DataType::CFrame: CompoundType = "CFrame"; break;
				default: CompoundType.reset(); break;
			}
			return *this;
		}

		InstanceProperty &SetCategory(std::string category) {
			Category = std::move(category);
			return *this;
		}

		InstanceProperty &SetNumericRange(std::optional<double> minimum, std::optional<double> maximum) {
			Range = NumericRange{minimum, maximum};
			return *this;
		}

		InstanceProperty &SetEditorHint(std::string hint) {
			EditorHint = std::move(hint);
			return *this;
		}

		InstanceProperty &SetNativeEnumType(std::string enumType) {
			SemanticType = DataType::NativeEnum;
			WireType = "EnumItem";
			NativeEnumType = std::move(enumType);
			return *this;
		}

		InstanceProperty &SetObjectReferenceType(SchemaId classSchemaId, bool nullable) {
			SemanticType = DataType::ObjectReference;
			WireType = "ObjectReference";
			ObjectReferenceClassSchemaId = classSchemaId;
			Nullable = nullable;
			return *this;
		}

		[[nodiscard]] bool IsValueValid(const std::any &value) const;

		InstanceProperty &SetUnmodified(std::any unmodified) {
			Unmodified = unmodified;
			return *this;
		}

		InstanceProperty &SetSignal(bool isSignal = true) {
			Signal = isSignal;
			return *this;
		}

		InstanceProperty &SetSerializable(bool serializable = true) {
			PersistencePolicy = serializable ? Persistence::Saved : Persistence::Transient;
			return *this;
		}

		InstanceProperty &SetReplication(Replication replication) {
			ReplicationPolicy = replication;
			return *this;
		}

		InstanceProperty &SetMaterializationDependency(MaterializationDependency Dependency) {
			MaterializationDependencyPolicy = Dependency;
			return *this;
		}

		InstanceProperty &SetAuthority(Authority authority) {
			WriteAuthority = authority;
			return *this;
		}

		InstanceProperty &SetEditable(bool editable) {
			Editable = editable;
			return *this;
		}

		InstanceProperty &SetReadDomains(ScriptDomainSet domains) {
			ReadDomains = std::move(domains);
			return *this;
		}

		InstanceProperty &SetWriteDomains(ScriptDomainSet domains) {
			WriteDomains = std::move(domains);
			return *this;
		}

		InstanceProperty &SetRequiredReadCapability(ScriptCapability capability) {
			RequiredReadCapability = capability;
			return *this;
		}

		InstanceProperty &SetRequiredWriteCapability(ScriptCapability capability) {
			RequiredWriteCapability = capability;
			return *this;
		}

		[[nodiscard]] bool CanRead(const ScriptSecurityContext &context) const {
			return ReadDomains.Contains(context.Domain) && context.HasCapability(RequiredReadCapability);
		}

		[[nodiscard]] bool CanWrite(const ScriptSecurityContext &context) const {
			return WriteDomains.Contains(context.Domain) && context.HasCapability(RequiredWriteCapability);
		}

		InstanceProperty &SetValidator(std::function<bool(const std::any &)> validator) {
			Validate = std::move(validator);
			return *this;
		}

		InstanceProperty &SetReadPermission(Enums::Permission permission) {
			ReadPermission = permission;
			return *this;
		}

		InstanceProperty &SetWritePermission(Enums::Permission permission) {
			WritePermission = permission;
			return *this;
		}

		std::pair<std::string, InstanceProperty> IntoPair() && {
			std::string propName = std::move(Name);
			InstanceProperty prop = std::move(*this);
			prop.Name = propName;
			return {propName, std::move(prop)};
		}

		std::pair<std::string, InstanceProperty> IntoPair() & {
			std::string propName = std::move(Name);
			InstanceProperty prop = std::move(*this);
			prop.Name = propName;
			return {propName, std::move(prop)};
		}

		template <auto Pointer> InstanceProperty &UseRead() {
			using Traits = MemberPointerTraits<decltype(Pointer)>;
			using ClassType = typename Traits::ClassType;
			using MemberType = typename Traits::MemberType;

			if constexpr (std::is_member_function_pointer_v<decltype(Pointer)>) {
				Read = [](Instance *self) -> std::any {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					return (obj->*Pointer)();
				};
			} else if constexpr (std::is_member_object_pointer_v<decltype(Pointer)>) {
				Read = [](Instance *self) -> std::any {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					return obj->*Pointer;
				};
			}

			if constexpr (SharedSignalPointer<MemberType>::value) {
				ReadSignal = [](Instance *self) {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					if constexpr (std::is_member_function_pointer_v<decltype(Pointer)>)
						return std::static_pointer_cast<BaseSignal>((obj->*Pointer)());
					else return std::static_pointer_cast<BaseSignal>(obj->*Pointer);
				};
			} else if constexpr (SharedInstancePointer<MemberType>::value) {
				ReadObjectReference = [](Instance *self) {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					if constexpr (std::is_member_function_pointer_v<decltype(Pointer)>) {
						return std::static_pointer_cast<Instance>((obj->*Pointer)());
					} else {
						return std::static_pointer_cast<Instance>(obj->*Pointer);
					}
				};
			} else if constexpr (OptionalSharedInstancePointer<MemberType>::value) {
				ReadObjectReference = [](Instance *self) -> std::shared_ptr<Instance> {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					MemberType value;
					if constexpr (std::is_member_function_pointer_v<decltype(Pointer)>) value = (obj->*Pointer)();
					else value = obj->*Pointer;
					return value ? std::static_pointer_cast<Instance>(*value) : nullptr;
				};
			} else if constexpr (std::is_enum_v<MemberType>) {
				ReadEnumValue = [](Instance *self) {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					MemberType value;
					if constexpr (std::is_member_function_pointer_v<decltype(Pointer)>) value = (obj->*Pointer)();
					else value = obj->*Pointer;
					return std::pair(std::string(magic_enum::enum_type_name<MemberType>()), static_cast<int>(value));
				};
				ReadEncodedEnumValue = [](const std::any &value) -> std::optional<int> {
					if (auto typed = std::any_cast<MemberType>(&value)) return static_cast<int>(*typed);
					return std::nullopt;
				};
			}

			PushStack = [](lua_State *L, const std::any &value) -> int {
				if (auto val = std::any_cast<MemberType>(&value)) return StackValue<MemberType>::Push(L, *val);
				return 0;
			};

			return *this;
		};

		template <auto Pointer> InstanceProperty &UseWrite() {
			using Traits = MemberPointerTraits<decltype(Pointer)>;
			using ClassType = typename Traits::ClassType;

			if constexpr (std::is_member_function_pointer_v<decltype(Pointer)>) {
				using RawArgType = std::tuple_element_t<0, typename Traits::ArgsType>;
				using ArgType = std::decay_t<RawArgType>;

				Write = [](Instance *self, const std::any &value) {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					if (auto val = std::any_cast<ArgType>(&value)) {
						(obj->*Pointer)(*val);
						return;
					}
					throw std::invalid_argument("Reflected property value has the wrong native type");
				};

				IsStack = [](lua_State *L, int idx) { return StackValue<ArgType>::Is(L, idx); };
				FromStack = [](lua_State *L, int idx) { return StackValue<ArgType>::From(L, idx); };

				if constexpr (SharedInstancePointer<ArgType>::value) {
					using Element = typename SharedInstancePointer<ArgType>::Element;
					DecodeObjectReference = [](const std::any &value) -> std::shared_ptr<Instance> {
						auto typed = std::any_cast<ArgType>(&value);
						if (!typed) throw std::invalid_argument("Object reference has the wrong native type");
						return std::static_pointer_cast<Instance>(*typed);
					};
					WriteObjectReference = [](Instance *self, std::shared_ptr<Instance> value) {
						ClassType *obj = reinterpret_cast<ClassType *>(self);
						auto typed = std::dynamic_pointer_cast<Element>(value);
						if (value && !typed) throw std::invalid_argument("Object reference has the wrong class");
						(obj->*Pointer)(typed);
					};
				} else if constexpr (OptionalSharedInstancePointer<ArgType>::value) {
					using Element = typename OptionalSharedInstancePointer<ArgType>::Element;
					DecodeObjectReference = [](const std::any &value) -> std::shared_ptr<Instance> {
						auto typed = std::any_cast<ArgType>(&value);
						if (!typed) throw std::invalid_argument("Object reference has the wrong native type");
						return *typed ? std::static_pointer_cast<Instance>(**typed) : nullptr;
					};
					WriteObjectReference = [](Instance *self, std::shared_ptr<Instance> value) {
						ClassType *obj = reinterpret_cast<ClassType *>(self);
						auto typed = std::dynamic_pointer_cast<Element>(value);
						if (value && !typed) throw std::invalid_argument("Object reference has the wrong class");
						(obj->*Pointer)(typed ? std::optional(typed) : std::nullopt);
					};
				} else if constexpr (std::is_enum_v<ArgType>) {
					WriteEnumValue = [](Instance *self, int value) {
						ClassType *obj = reinterpret_cast<ClassType *>(self);
						(obj->*Pointer)(static_cast<ArgType>(value));
					};
				}
			} else {
				using MemberType = typename Traits::MemberType;

				Write = [](Instance *self, const std::any &value) {
					ClassType *obj = reinterpret_cast<ClassType *>(self);
					if (auto val = std::any_cast<MemberType>(&value)) {
						obj->*Pointer = *val;
						return;
					}
					throw std::invalid_argument("Reflected property value has the wrong native type");
				};

				IsStack = [](lua_State *L, int idx) { return StackValue<MemberType>::Is(L, idx); };
				FromStack = [](lua_State *L, int idx) { return StackValue<MemberType>::From(L, idx); };

				if constexpr (SharedInstancePointer<MemberType>::value) {
					using Element = typename SharedInstancePointer<MemberType>::Element;
					DecodeObjectReference = [](const std::any &value) -> std::shared_ptr<Instance> {
						auto typed = std::any_cast<MemberType>(&value);
						if (!typed) throw std::invalid_argument("Object reference has the wrong native type");
						return std::static_pointer_cast<Instance>(*typed);
					};
					WriteObjectReference = [](Instance *self, std::shared_ptr<Instance> value) {
						ClassType *obj = reinterpret_cast<ClassType *>(self);
						auto typed = std::dynamic_pointer_cast<Element>(value);
						if (value && !typed) throw std::invalid_argument("Object reference has the wrong class");
						obj->*Pointer = typed;
					};
				} else if constexpr (OptionalSharedInstancePointer<MemberType>::value) {
					using Element = typename OptionalSharedInstancePointer<MemberType>::Element;
					DecodeObjectReference = [](const std::any &value) -> std::shared_ptr<Instance> {
						auto typed = std::any_cast<MemberType>(&value);
						if (!typed) throw std::invalid_argument("Object reference has the wrong native type");
						return *typed ? std::static_pointer_cast<Instance>(**typed) : nullptr;
					};
					WriteObjectReference = [](Instance *self, std::shared_ptr<Instance> value) {
						ClassType *obj = reinterpret_cast<ClassType *>(self);
						auto typed = std::dynamic_pointer_cast<Element>(value);
						if (value && !typed) throw std::invalid_argument("Object reference has the wrong class");
						obj->*Pointer = typed ? std::optional(typed) : std::nullopt;
					};
				}
			}

			return *this;
		};
	};
}
