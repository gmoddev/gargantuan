#pragma once

#include "gargantuan/InstanceClassDefinition.hpp"
#include "gargantuan/classes/generated/Instance.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/WireValue.hpp"
#include "gargantuan/scripting/Userdata.hpp"

#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gargantuan {
	class DataModel;
	[[nodiscard]] InstanceProperty MakeCustomClassInstanceProperty(
		SchemaId declaringClassId,
		std::uint32_t definitionVersion,
		const SchemaClassProperty &property
	);
	G_SHARED_USERDATA_DECL(
		Instance, I_Instance;

		Instance();
		virtual ~Instance();

		std::vector<std::shared_ptr<Instance>> Children;
		std::unordered_map<std::string, std::shared_ptr<Signal<std::monostate>>> PropertyChangedSignals;

	  private:
		std::map<std::string, WireValue> Attributes;
		std::map<SchemaId, std::map<std::string, WireValue>> ExtensionValues;
		std::map<SchemaId, std::map<std::string, WireValue>> CustomPropertyValues;
		SchemaId RuntimeSchemaClassId{};
		std::unordered_map<std::string, std::shared_ptr<Signal<std::monostate>>> AttributeChangedSignals;

	  public:
		std::weak_ptr<Instance> ParentReference;
		std::weak_ptr<DataModel> OwningDataModel;
		mutable ObjectId Id;
		mutable std::mutex IdentityMutex;
		bool DestroyingState = false;

		const InstanceProperty *FindProperty(std::string name);
		[[nodiscard]] std::optional<WireValue> ReadPropertyWireValue(std::string_view PropertyName) const;
		const Self::Method *FindMethod(std::string name);
		static int LIndex(lua_State *L, Instance *instance);
		static int LNewIndex(lua_State *L, Instance *instance);
		static int LNamecall(lua_State *L, Instance *instance);

		typedef std::function<void(std::shared_ptr<gargantuan::Instance> instance)> BindCallback;
		std::function<void()> BindChildren(BindCallback callback);
		std::function<void()> BindDescendants(BindCallback callback);

		void CollectDescendants(std::vector<std::shared_ptr<Instance>> &descendants);
		void FireAncestryChanged(std::shared_ptr<Instance> child, std::shared_ptr<Instance> parent);
		void AssertIsAlive() const;
		void NotifyPropertyCommitted(std::string_view propertyName);
		[[nodiscard]] ObjectId GetReplicationScopeId() const;
		[[nodiscard]] std::shared_ptr<DataModel> GetDataModel() const;
		void PublishReplicationSubtree(ObjectId scope);
		void MarkPersistenceSubtreeArchivable();
		void AssertCanMutate() const;
		void ValidatePropertyMutation(std::string_view propertyName, const std::any &value) const;
		MutationStatus ApplyPropertyMutation(
			std::string_view propertyName,
			const std::any &value,
			Enums::Permission permission = Enums::Permission::None,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		);
		[[nodiscard]] std::optional<WireValue> GetAttributeValue(
			std::string_view name,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		) const;
		[[nodiscard]] std::map<std::string, WireValue> GetAttributeValues(
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		) const;
		std::shared_ptr<Signal<std::monostate>> GetAttributeSignal(
			std::string_view name,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		);
		MutationStatus ApplyAttributeMutation(
			std::string_view name,
			std::optional<WireValue> value,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		);
		[[nodiscard]] WireValue GetExtensionPropertyValue(
			SchemaId extensionId,
			std::string_view propertyName,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		) const;
		[[nodiscard]] std::map<SchemaId, std::map<std::string, WireValue>> GetExtensionPropertyOverrides(
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		) const;
		MutationStatus ApplyExtensionPropertyMutation(
			SchemaId extensionId,
			std::uint32_t definitionVersion,
			std::string_view propertyName,
			WireValue value,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		);
		[[nodiscard]] WireValue GetCustomClassPropertyValue(
			SchemaId declaringClassId,
			std::string_view propertyName,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		) const;
		[[nodiscard]] std::map<SchemaId, std::map<std::string, WireValue>> GetCustomClassPropertyOverrides(
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		) const;
		MutationStatus ApplyCustomClassPropertyMutation(
			SchemaId declaringClassId,
			std::uint32_t definitionVersion,
			std::string_view propertyName,
			WireValue value,
			const ScriptSecurityContext &securityContext = GetCurrentScriptSecurityContext()
		);
		[[nodiscard]] SchemaId GetRuntimeSchemaClassId() const { return RuntimeSchemaClassId; }
		void BindRuntimeSchemaClass(SchemaId classId);
		[[nodiscard]] ObjectId GetObjectId() const;
		[[nodiscard]] bool IsDestroying() const { return DestroyingState; }
	);

	template <typename Subclass>
		requires std::is_base_of_v<Instance, Subclass> && (!std::is_same_v<Instance, Subclass>)
	struct StackValue<std::shared_ptr<Subclass>> {
	  public:
		static inline std::string_view ReflectedTypedef() {
			return Subclass::CLASS_DEFINITION.ClassName;
		};

		static bool Is(lua_State *L, int idx) {
			if (!StackValue<std::shared_ptr<Instance>>::Is(L, idx)) return false;
			auto instance = StackValue<std::shared_ptr<Instance>>::From(L, idx);
			return instance && instance->IsA(Subclass::CLASS_DEFINITION.ClassName);
		};

		static std::shared_ptr<Subclass> From(lua_State *L, int idx) {
			auto instance = gargantuan::StackValue<std::shared_ptr<Instance>>::From(L, idx);
			return instance ? std::dynamic_pointer_cast<Subclass>(instance) : nullptr;
		};

		static int Push(lua_State *L, std::shared_ptr<Subclass> value) {
			return gargantuan::StackValue<std::shared_ptr<Instance>>::Push(
				L, std::static_pointer_cast<Instance>(value)
			);
		};
	};
}
