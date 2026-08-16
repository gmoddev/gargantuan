#include "gargantuan/network/RemoteLuau.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace gargantuan::network {
	namespace {
		WireValue ReadArgument(lua_State *L, int Index) {
			if (lua_isnil(L, Index)) return std::monostate{};
			if (lua_isboolean(L, Index)) return lua_toboolean(L, Index) != 0;
			if (lua_isnumber(L, Index)) {
				const auto Value = static_cast<double>(lua_tonumber(L, Index));
				if (!std::isfinite(Value)) throw std::invalid_argument("Remote number must be finite");
				if (std::trunc(Value) == Value && Value >= std::numeric_limits<int>::min() &&
					Value <= std::numeric_limits<int>::max())
					return static_cast<int>(Value);
				return Value;
			}
			if (lua_isstring(L, Index)) {
				std::size_t Length = 0;
				const auto *Value = lua_tolstring(L, Index, &Length);
				if (Length > MaximumRemoteStringBytes) throw std::invalid_argument("Remote string exceeds 16 KiB");
				std::string Result(Value, Length);
				ValidateProtocolString(Result, MaximumRemoteStringBytes, "Remote string");
				return Result;
			}
			if (StackValue<std::shared_ptr<Instance>>::Is(L, Index)) {
				auto InstanceValue = StackValue<std::shared_ptr<Instance>>::From(L, Index);
				if (!InstanceValue || InstanceValue->IsDestroying())
					throw std::invalid_argument("Remote Instance reference is destroyed");
				return WireObjectReference{WireObjectId::FromObjectId(InstanceValue->GetObjectId())};
			}
			if (StackValue<Vector2>::Is(L, Index)) {
				const auto Value = StackValue<Vector2>::From(L, Index).Value;
				return WireVector2{Value.x, Value.y};
			}
			if (StackValue<glm::vec3>::Is(L, Index)) {
				const auto Value = StackValue<glm::vec3>::From(L, Index);
				return WireVector3{Value.x, Value.y, Value.z};
			}
			if (StackValue<Color3>::Is(L, Index)) {
				const auto Value = StackValue<Color3>::From(L, Index);
				return WireColor3{Value.R, Value.G, Value.B};
			}
			if (StackValue<UDim>::Is(L, Index)) {
				const auto Value = StackValue<UDim>::From(L, Index);
				return WireUDim{Value.Scale, Value.Offset};
			}
			if (StackValue<UDim2>::Is(L, Index)) {
				const auto Value = StackValue<UDim2>::From(L, Index);
				return WireUDim2{{Value.X.Scale, Value.X.Offset}, {Value.Y.Scale, Value.Y.Offset}};
			}
			if (StackValue<CFrame>::Is(L, Index)) {
				const auto Value = StackValue<CFrame>::From(L, Index);
				const auto Components = Value.GetComponents();
				WireCFrame Result;
				std::apply(
					[&](const auto... Component) {
						std::size_t OutputIndex = 0;
						((Result.Components[OutputIndex++] = static_cast<float>(Component)), ...);
					},
					Components
				);
				return Result;
			}
			if (StackValue<EnumItem>::Is(L, Index)) {
				const auto Value = StackValue<EnumItem>::From(L, Index);
				if (!Value.EnumType) throw std::invalid_argument("Remote EnumItem has no enum type");
				return WireEnumItem{std::string(Value.EnumType->Name), std::string(Value.Name)};
			}
			throw std::invalid_argument("Remote argument type is unsupported");
		}

		int PushArgument(lua_State *L, const WireValue &Argument, const RemoteManager::ObjectResolver &ResolveObject) {
			return std::visit(
				[&](const auto &Value) -> int {
					using Type = std::decay_t<decltype(Value)>;
					if constexpr (std::is_same_v<Type, std::monostate>) {
						lua_pushnil(L);
						return 1;
					} else if constexpr (std::is_same_v<Type, bool>)
						return StackValue<bool>::Push(L, Value);
					else if constexpr (std::is_same_v<Type, int>)
						return StackValue<int>::Push(L, Value);
					else if constexpr (std::is_same_v<Type, double>)
						return StackValue<double>::Push(L, Value);
					else if constexpr (std::is_same_v<Type, WireFloat>)
						return StackValue<float>::Push(L, Value.Value);
					else if constexpr (std::is_same_v<Type, std::string>)
						return StackValue<std::string>::Push(L, Value);
					else if constexpr (std::is_same_v<Type, WireVector2>)
						return StackValue<Vector2>::Push(L, Vector2(Value.X, Value.Y));
					else if constexpr (std::is_same_v<Type, WireVector3>)
						return StackValue<glm::vec3>::Push(L, {Value.X, Value.Y, Value.Z});
					else if constexpr (std::is_same_v<Type, WireColor3>)
						return StackValue<Color3>::Push(L, Color3(Value.R, Value.G, Value.B));
					else if constexpr (std::is_same_v<Type, WireUDim>)
						return StackValue<UDim>::Push(L, UDim(Value.Scale, Value.Offset));
					else if constexpr (std::is_same_v<Type, WireUDim2>)
						return StackValue<UDim2>::Push(
							L, UDim2(Value.X.Scale, Value.X.Offset, Value.Y.Scale, Value.Y.Offset)
						);
					else if constexpr (std::is_same_v<Type, WireCFrame>) {
						const auto &C = Value.Components;
						return StackValue<CFrame>::Push(
							L, CFrame(C[0], C[1], C[2], C[3], C[4], C[5], C[6], C[7], C[8], C[9], C[10], C[11])
						);
					} else if constexpr (std::is_same_v<Type, WireEnumItem>) {
						auto Enum = Enums::GetEnums().find(Value.EnumType);
						if (Enum == Enums::GetEnums().end())
							throw std::runtime_error("Remote enum type is unavailable");
						auto Item = Enum->second->FromName(Value.Item);
						if (!Item) throw std::runtime_error("Remote enum item is unavailable");
						return StackValue<EnumItem>::Push(L, *Item);
					} else if constexpr (std::is_same_v<Type, WireObjectReference>) {
						auto InstanceValue = ResolveObject ? ResolveObject(Value.Object.ToObjectId()) : nullptr;
						if (!InstanceValue) throw std::runtime_error("Remote Object reference is unresolved");
						return StackValue<std::shared_ptr<Instance>>::Push(L, std::move(InstanceValue));
					} else {
						throw std::runtime_error("Remote schema enum values are not exposed to Luau yet");
					}
				},
				Argument
			);
		}
	}

	std::vector<WireValue> ReadRemoteLuauArguments(lua_State *L, int FirstIndex) {
		const int Top = lua_gettop(L);
		const int Count = std::max(Top - FirstIndex + 1, 0);
		if (Count > MaximumRemoteArguments) throw std::invalid_argument("Remote argument count exceeds 32");
		std::vector<WireValue> Result;
		Result.reserve(Count);
		for (int Index = FirstIndex; Index <= Top; ++Index)
			Result.push_back(ReadArgument(L, Index));
		return Result;
	}

	void ValidateRemoteLuauArguments(
		std::span<const WireValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	) {
		if (Arguments.size() > MaximumRemoteArguments) throw std::invalid_argument("Remote argument count exceeds 32");
		for (const auto &Argument : Arguments) {
			if (std::holds_alternative<WireSchemaEnumValue>(Argument))
				throw std::invalid_argument("Remote schema enum values are not exposed to Luau");
			if (const auto *Value = std::get_if<WireEnumItem>(&Argument)) {
				auto Enum = Enums::GetEnums().find(Value->EnumType);
				if (Enum == Enums::GetEnums().end() || !Enum->second->FromName(Value->Item))
					throw std::invalid_argument("Remote EnumItem is unavailable");
			}
			if (const auto *Reference = std::get_if<WireObjectReference>(&Argument);
				Reference && (!ResolveObject || !ResolveObject(Reference->Object.ToObjectId())))
				throw std::invalid_argument("Remote Object reference is unresolved");
		}
	}

	RemoteManager::ObjectResolver StabilizeRemoteLuauObjectReferences(
		std::span<const WireValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	) {
		ValidateRemoteLuauArguments(Arguments, ResolveObject);
		std::vector<std::shared_ptr<Instance>> Objects;
		Objects.reserve(Arguments.size());
		for (const auto &Argument : Arguments) {
			const auto *Reference = std::get_if<WireObjectReference>(&Argument);
			if (!Reference) continue;
			auto Object = ResolveObject(Reference->Object.ToObjectId());
			if (!Object) throw std::invalid_argument("Remote Object reference is unresolved");
			Objects.push_back(std::move(Object));
		}
		return [Objects = std::move(Objects)](ObjectId Id) -> std::shared_ptr<Instance> {
			for (const auto &Object : Objects)
				if (Object && Object->GetObjectId() == Id) return Object;
			return nullptr;
		};
	}

	int PushRemoteLuauArguments(
		lua_State *L, std::span<const WireValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	) {
		if (Arguments.size() > MaximumRemoteArguments || !lua_checkstack(L, static_cast<int>(Arguments.size())))
			throw std::runtime_error("Remote arguments exceed Luau stack capacity");
		int Pushed = 0;
		for (const auto &Argument : Arguments)
			Pushed += PushArgument(L, Argument, ResolveObject);
		return Pushed;
	}

	void PushRemotePeerContext(lua_State *L, const RemotePeerContext &Peer) {
		lua_createtable(L, 0, 3);
		lua_pushinteger(L, Peer.Connection.Slot);
		lua_setfield(L, -2, "Slot");
		lua_pushinteger(L, Peer.Connection.Generation);
		lua_setfield(L, -2, "Generation");
		lua_pushnumber(L, static_cast<double>(Peer.Epoch.Value()));
		lua_setfield(L, -2, "Epoch");
		lua_setreadonly(L, -1, true);
	}
}
