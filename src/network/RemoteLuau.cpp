#include "gargantuan/network/RemoteLuau.hpp"

#include "RemoteValueBudget.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include "../scripting/LuauTableStorage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace gargantuan::network {
	namespace {
		void RequireBudget(bool WithinBudget) {
			if (!WithinBudget) throw std::invalid_argument("Remote values exceed a payload, allocation, or reference limit");
		}

		RemoteValue ReadScalar(
			lua_State *L, int Index, bool TopLevel, const RemoteManager *Manager, detail::RemoteValueBudget &Budget
		) {
			if (lua_isnil(L, Index)) return std::monostate{};
			if (lua_isboolean(L, Index)) return lua_toboolean(L, Index) != 0;
			if (lua_type(L, Index) == LUA_TSTRING) {
				std::size_t Length = 0;
				const auto *Text = lua_tolstring(L, Index, &Length);
				if (Length > MaximumRemoteStringBytes) throw std::invalid_argument("Remote string exceeds 16 KiB");
				if (std::memchr(Text, 0, Length))
					throw std::invalid_argument("Remote string contains an embedded null");
			}
			// Preserve the established numeric-string conversion for positional
			// arguments. Nested data is a raw typed snapshot.
			if (lua_type(L, Index) == LUA_TNUMBER || (TopLevel && lua_isnumber(L, Index))) {
				const auto Value = static_cast<double>(lua_tonumber(L, Index));
				if (!std::isfinite(Value)) throw std::invalid_argument("Remote number must be finite");
				if (std::trunc(Value) == Value && Value >= std::numeric_limits<int>::min() &&
					Value <= std::numeric_limits<int>::max())
					return static_cast<int>(Value);
				return Value;
			}
			if (lua_type(L, Index) == LUA_TSTRING) {
				std::size_t Length = 0;
				const auto *Value = lua_tolstring(L, Index, &Length);
				const std::string_view Text(Value, Length);
				RequireBudget(detail::ChargeRemoteScalar(Budget, Text));
				ValidateProtocolString(Text, MaximumRemoteStringBytes, "Remote string");
				return std::string(Text);
			}
			if (StackValue<std::shared_ptr<Instance>>::Is(L, Index)) {
				auto InstanceValue = StackValue<std::shared_ptr<Instance>>::From(L, Index);
				if (!InstanceValue || InstanceValue->IsDestroying() || InstanceValue->GetDestroyed())
					throw std::invalid_argument("Remote Instance reference is destroyed");
				const auto Object = Manager ? Manager->MapOutgoingReference(*InstanceValue) :
					std::optional(InstanceValue->GetObjectId());
				if (!Object || !Object->IsValid())
					throw std::invalid_argument("Remote Instance reference has no current network identity");
				return WireObjectReference{WireObjectId::FromObjectId(*Object)};
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

		struct ArgumentReader {
			lua_State *L;
			const RemoteManager *Manager;
			detail::RemoteValueBudget Budget;
			std::array<const void *, MaximumRemoteContainerDepth> Ancestors{};
			std::size_t RemainingTableSlots = MaximumRemoteLuauTableSlots;

			void ChargeTableTraversal(std::size_t Slots) {
				if (Slots > RemainingTableSlots)
					throw std::invalid_argument("Remote tables exceed the cumulative Luau storage scan limit");
				RemainingTableSlots -= Slots;
			}

			RemoteValue Read(int Index, std::size_t Depth) {
				RequireBudget(Budget.Value());
				if (lua_istable(L, Index)) return ReadTable(lua_absindex(L, Index), Depth);
				if (lua_isbuffer(L, Index)) {
					std::size_t Length = 0;
					const auto *Bytes = static_cast<const std::byte *>(lua_tobuffer(L, Index, &Length));
					if (Length > MaximumRemoteBufferBytes) throw std::invalid_argument("Remote buffer exceeds 64 KiB");
					RequireBudget(Budget.Wire(4 + Length) && Budget.Storage(Length));
					RemoteBuffer Result;
					if (Length != 0) Result.Bytes.assign(Bytes, Bytes + Length);
					return Result;
				}
				auto Result = ReadScalar(L, Index, Depth == 0, Manager, Budget);
				std::visit([&](const auto &Value) {
					using Type = std::decay_t<decltype(Value)>;
					if constexpr (!std::is_same_v<Type, RemoteArray> && !std::is_same_v<Type, RemoteDictionary> &&
						!std::is_same_v<Type, RemoteBuffer> && !std::is_same_v<Type, std::string>)
						RequireBudget(detail::ChargeRemoteScalar(Budget, Value));
				}, Result);
				return Result;
			}

			RemoteValue ReadTable(int Index, std::size_t Depth) {
				if (Depth >= MaximumRemoteContainerDepth)
					throw std::invalid_argument("Remote table exceeds eight levels");
				if (lua_getmetatable(L, Index)) throw std::invalid_argument("Remote tables cannot have metatables");
				const auto *Identity = lua_topointer(L, Index);
				if (std::find(Ancestors.begin(), Ancestors.begin() + Depth, Identity) != Ancestors.begin() + Depth)
					throw std::invalid_argument("Remote tables cannot contain cycles");
				Ancestors[Depth] = Identity;
				RequireBudget(Budget.Wire(2));
				// Raw iteration scans empty backing slots as well as live entries.
				// Read the bundled Luau layout only to cap that native work.
				const std::size_t Slots = LuauTableStorageSlots(Identity);
				ChargeTableTraversal(Slots);

				std::size_t Count = 0;
				std::size_t LastIndex = 0;
				bool HasKeys = false;
				for (int Cursor = lua_rawiter(L, Index, 0); Cursor >= 0; Cursor = lua_rawiter(L, Index, Cursor)) {
					RequireBudget(++Count <= MaximumRemoteValueCount - Budget.ValueCount);
					const int KeyType = lua_type(L, -2);
					if (KeyType == LUA_TNUMBER) {
						const double Key = lua_tonumber(L, -2);
						if (!(Key >= 1 && Key <= MaximumRemoteValueCount) || std::trunc(Key) != Key)
							throw std::invalid_argument("Remote arrays require consecutive positive integer keys");
						LastIndex = std::max(LastIndex, static_cast<std::size_t>(Key));
					} else if (KeyType == LUA_TSTRING) {
						HasKeys = true;
					} else {
						throw std::invalid_argument("Remote table keys must be array indices or strings");
					}
					if (HasKeys && LastIndex != 0)
						throw std::invalid_argument("Remote tables cannot mix array indices and string keys");
					lua_pop(L, 2);
				}
				if (!HasKeys) {
					if (LastIndex != Count) throw std::invalid_argument("Remote arrays cannot contain holes");
					RequireBudget(Budget.Children(Count, sizeof(RemoteValue)));
					RemoteArray Result;
					Result.Values.resize(Count);
					if (Count != 0) {
						ChargeTableTraversal(Slots);
						for (int Cursor = lua_rawiter(L, Index, 0); Cursor >= 0; Cursor = lua_rawiter(L, Index, Cursor)) {
							const auto Key = static_cast<std::size_t>(lua_tonumber(L, -2));
							Result.Values[Key - 1] = Read(-1, Depth + 1);
							lua_pop(L, 2);
						}
					}
					return Result;
				}

				RequireBudget(Budget.Children(Count, sizeof(RemoteDictionaryEntry)));
				ChargeTableTraversal(Slots);
				RemoteDictionary Result;
				Result.Entries.reserve(Count);
				for (int Cursor = lua_rawiter(L, Index, 0); Cursor >= 0; Cursor = lua_rawiter(L, Index, Cursor)) {
					std::size_t Length = 0;
					const auto *KeyBytes = lua_tolstring(L, -2, &Length);
					const std::string_view Key(KeyBytes, Length);
					ValidateProtocolString(Key, MaximumRemoteDictionaryKeyBytes, "Remote dictionary key");
					RequireBudget(Budget.Wire(4 + Length) && Budget.Storage(Length));
					Result.Entries.push_back({std::string(Key), Read(-1, Depth + 1)});
					lua_pop(L, 2);
				}
				std::ranges::sort(Result.Entries, {}, &RemoteDictionaryEntry::Key);
				return Result;
			}
		};

		std::shared_ptr<Instance> ResolveLuauReference(
			ObjectId Object, const RemoteManager::ObjectResolver &ResolveObject
		) {
			auto InstanceValue = ResolveObject ? ResolveObject(Object) : nullptr;
			if (!InstanceValue || InstanceValue->GetDestroyed() || InstanceValue->IsDestroying())
				throw std::invalid_argument("Remote Object reference is unresolved");
			return InstanceValue;
		}

		int PushArgument(lua_State *L, const RemoteValue &Argument, const RemoteManager::ObjectResolver &ResolveObject) {
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
					} else if constexpr (std::is_same_v<Type, RemoteArray>) {
						lua_createtable(L, static_cast<int>(Value.Values.size()), 0);
						int Index = 1;
						for (const auto &Entry : Value.Values) {
							PushArgument(L, Entry, ResolveObject);
							lua_rawseti(L, -2, Index++);
						}
						return 1;
					} else if constexpr (std::is_same_v<Type, RemoteDictionary>) {
						lua_createtable(L, 0, static_cast<int>(Value.Entries.size()));
						for (const auto &Entry : Value.Entries) {
							lua_pushlstring(L, Entry.Key.data(), Entry.Key.size());
							PushArgument(L, Entry.Value, ResolveObject);
							lua_rawset(L, -3);
						}
						return 1;
					} else if constexpr (std::is_same_v<Type, RemoteBuffer>) {
						auto *Destination = lua_newbuffer(L, Value.Bytes.size());
						if (!Value.Bytes.empty()) std::memcpy(Destination, Value.Bytes.data(), Value.Bytes.size());
						return 1;
					} else if constexpr (std::is_same_v<Type, WireObjectReference>) {
						return StackValue<std::shared_ptr<Instance>>::Push(
							L, ResolveLuauReference(Value.Object.ToObjectId(), ResolveObject)
						);
					} else {
						throw std::runtime_error("Remote schema enum values are not exposed to Luau yet");
					}
				},
				Argument
			);
		}
	}

	std::vector<RemoteValue> ReadRemoteLuauArguments(lua_State *L, int FirstIndex, const RemoteManager *Manager) {
		const int Top = lua_gettop(L);
		const int Count = std::max(Top - FirstIndex + 1, 0);
		if (Count > MaximumRemoteArguments) throw std::invalid_argument("Remote argument count exceeds 32");
		if (!lua_checkstack(L, static_cast<int>(2 * MaximumRemoteContainerDepth + 2)))
			throw std::invalid_argument("Remote table exceeds Luau stack capacity");
		ArgumentReader Reader{.L = L, .Manager = Manager};
		RequireBudget(Reader.Budget.Children(Count, sizeof(RemoteValue)));
		std::vector<RemoteValue> Result;
		Result.reserve(Count);
		try {
			for (int Index = FirstIndex; Index <= Top; ++Index)
				Result.push_back(Reader.Read(Index, 0));
		} catch (...) {
			lua_settop(L, Top);
			throw;
		}
		return Result;
	}

	namespace {
		void ValidateLuauValue(const RemoteValue &Argument) {
			if (std::holds_alternative<WireSchemaEnumValue>(Argument))
				throw std::invalid_argument("Remote schema enum values are not exposed to Luau");
			if (const auto *Value = std::get_if<WireEnumItem>(&Argument)) {
				auto Enum = Enums::GetEnums().find(Value->EnumType);
				if (Enum == Enums::GetEnums().end() || !Enum->second->FromName(Value->Item))
					throw std::invalid_argument("Remote EnumItem is unavailable");
			}
		}

	}

	void ValidateRemoteLuauArguments(
		std::span<const RemoteValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	) {
		for (const auto &Argument : Arguments)
			detail::VisitRemoteValues(Argument, [&](const RemoteValue &Value) {
				ValidateLuauValue(Value);
				if (const auto *Reference = std::get_if<WireObjectReference>(&Value))
					(void)ResolveLuauReference(Reference->Object.ToObjectId(), ResolveObject);
				return true;
			});
	}

	RemoteManager::ObjectResolver StabilizeRemoteLuauObjectReferences(
		std::span<const RemoteValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	) {
		using ResolvedReferences = std::map<ObjectId, std::shared_ptr<Instance>>;
		std::shared_ptr<ResolvedReferences> Objects;
		for (const auto &Argument : Arguments)
			detail::VisitRemoteValues(Argument, [&](const RemoteValue &Value) {
				ValidateLuauValue(Value);
				if (const auto *Reference = std::get_if<WireObjectReference>(&Value)) {
					const auto Object = Reference->Object.ToObjectId();
					if (!Objects) Objects = std::make_shared<ResolvedReferences>();
					if (!Objects->contains(Object)) Objects->emplace(Object, ResolveLuauReference(Object, ResolveObject));
				}
				return true;
			});
		if (!Objects) return {};
		return [Objects = std::shared_ptr<const ResolvedReferences>(std::move(Objects))](ObjectId Id)
			-> std::shared_ptr<Instance> {
			const auto Found = Objects->find(Id);
			return Found != Objects->end() ? Found->second : nullptr;
		};
	}

	int PushRemoteLuauArguments(
		lua_State *L, std::span<const RemoteValue> Arguments, const RemoteManager::ObjectResolver &ResolveObject
	) {
		const auto Top = lua_gettop(L);
		if (Arguments.size() > MaximumRemoteArguments ||
			!lua_checkstack(L, static_cast<int>(Arguments.size() + 2 * MaximumRemoteContainerDepth + 2)))
			throw std::runtime_error("Remote arguments exceed Luau stack capacity");
		int Pushed = 0;
		try {
			for (const auto &Argument : Arguments)
				Pushed += PushArgument(L, Argument, ResolveObject);
		} catch (...) {
			lua_settop(L, Top);
			throw;
		}
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
