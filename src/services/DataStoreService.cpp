#include "gargantuan/services/DataStoreService.hpp"

#include "../runtime/RuntimeWorkDiagnostics.hpp"
#include "../scripting/LuauTableStorage.hpp"
#include "gargantuan/persistence/PersistenceRuntime.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace gargantuan {
	namespace {
		using Value = PersistenceDocumentValue;
		constexpr char ArrayMetatable[] = "gargantuan.persistence.array";
		char NullSentinel;
		constexpr std::size_t MaximumTableSlots = 16384;

		[[noreturn]] void Invalid(const char* Detail) { throw std::invalid_argument(Detail); }

		ScriptEngine& RequireAccess(lua_State* L) {
			const auto& Context = GetCurrentScriptSecurityContext();
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main ||
				Context.Domain != ScriptExecutionDomain::Server || !Context.HasCapability(ScriptCapability::PersistenceAccess))
				throw std::runtime_error("[Persistence:Access] Server gameplay context required");
			auto* Engine = ScriptEngine::Get(L);
			if (!Engine || Engine->IsStopping() || Engine->Mode == RuntimeMode::NetworkClient)
				throw std::runtime_error("[Persistence:Access] A live server runtime is required");
			return *Engine;
		}

		std::shared_ptr<PersistenceRuntime> RequireRuntime(lua_State* L, Instance* Service) {
			const auto& Engine = RequireAccess(L);
			auto Runtime = Engine.GetPersistenceRuntime();
			if (!Runtime || Service->GetDestroyed() || Service->IsDestroying() ||
				Service->GetDataModel() != Engine.DataModel)
				throw std::runtime_error("[Persistence:Access] DataStoreService is unavailable");
			return Runtime;
		}

		std::shared_ptr<PersistenceRuntime> RequireRuntime(lua_State* L, const std::weak_ptr<PersistenceRuntime>& Bound) {
			auto Runtime = Bound.lock();
			if (!Runtime || Runtime != RequireAccess(L).GetPersistenceRuntime())
				throw std::runtime_error("[Persistence:Access] Handle belongs to another runtime");
			return Runtime;
		}

		std::string_view ReadStringView(lua_State* L, int Index) {
			if (lua_type(L, Index) != LUA_TSTRING) Invalid("[Persistence:Value] Expected a string");
			std::size_t Length = 0;
			const auto* Text = lua_tolstring(L, Index, &Length);
			return {Text, Length};
		}

		std::string ReadString(lua_State* L, int Index, std::size_t Maximum) {
			const auto Text = ReadStringView(L, Index);
			if (Text.size() > Maximum) Invalid("[Persistence:Value] String exceeds its byte limit");
			return std::string(Text);
		}

		void PushString(lua_State* L, std::string_view Text) { lua_pushlstring(L, Text.data(), Text.size()); }

		void PushArrayTable(lua_State* L, int Capacity) {
			lua_createtable(L, Capacity, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, ArrayMetatable);
			lua_setmetatable(L, -2);
		}

		class DocumentReader final {
		public:
			explicit DocumentReader(lua_State* L) : L(L) {
				if (!lua_checkstack(L, static_cast<int>(4 * PersistenceMaxDocumentDepth)))
					throw std::runtime_error("[Persistence:Value] Luau stack limit reached");
			}

			Value Read(int Index, std::size_t Depth = 1) {
				if (Depth > PersistenceMaxDocumentDepth || ++Nodes > PersistenceMaxDocumentNodes)
					Invalid("[Persistence:Value] Document depth or value count exceeds its limit");
				Charge(8);
				switch (lua_type(L, Index)) {
					case LUA_TBOOLEAN: return {lua_toboolean(L, Index) != 0};
					case LUA_TNUMBER: {
						const auto Number = lua_tonumber(L, Index);
						if (!std::isfinite(Number)) Invalid("[Persistence:Value] Documents require finite numbers");
						return {Number == 0.0 ? 0.0 : Number};
					}
					case LUA_TSTRING: {
						const auto Text = ReadStringView(L, Index);
						Charge(Text.size());
						std::string Copy(Text);
						if (Copy.find('\0') != std::string::npos || !IsPersistenceUtf8(Copy))
							Invalid("[Persistence:Value] Document strings require UTF-8 without NUL");
						return {std::move(Copy)};
					}
					case LUA_TLIGHTUSERDATA:
						if (lua_tolightuserdata(L, Index) == &NullSentinel) return {};
						break;
					case LUA_TTABLE: return ReadTable(lua_absindex(L, Index), Depth);
					default: break;
				}
				Invalid("[Persistence:Value] Unsupported value in a document");
			}

		private:
			lua_State* L;
			std::size_t Nodes = 0;
			std::size_t RemainingBytes = PersistenceMaxValueBytes;
			std::size_t RemainingSlots = MaximumTableSlots;
			std::array<const void*, PersistenceMaxDocumentDepth> Ancestors{};

			void Charge(std::size_t Bytes) {
				if (Bytes > RemainingBytes) Invalid("[Persistence:Value] Document exceeds its byte limit");
				RemainingBytes -= Bytes;
			}

			Value ReadTable(int Index, std::size_t Depth) {
				bool Array = false;
				if (lua_getmetatable(L, Index)) {
					lua_getfield(L, LUA_REGISTRYINDEX, ArrayMetatable);
					Array = lua_rawequal(L, -1, -2);
					lua_pop(L, 2);
					if (!Array) Invalid("[Persistence:Value] Document tables cannot use custom metatables");
				}
				const auto* Identity = lua_topointer(L, Index);
				const auto End = Ancestors.begin() + (Depth - 1);
				if (std::find(Ancestors.begin(), End, Identity) != End)
					Invalid("[Persistence:Value] Documents cannot contain table cycles");
				*End = Identity;
				const auto Slots = LuauTableStorageSlots(Identity);
				if (Slots > RemainingSlots / 2) Invalid("[Persistence:Value] Document table capacity exceeds its traversal limit");
				RemainingSlots -= 2 * Slots;

				std::size_t Count = 0;
				std::size_t Last = 0;
				bool HasFields = false;
				for (int Cursor = lua_rawiter(L, Index, 0); Cursor >= 0; Cursor = lua_rawiter(L, Index, Cursor)) {
					if (++Count > PersistenceMaxDocumentNodes - Nodes)
						Invalid("[Persistence:Value] Document exceeds its value count limit");
					if (lua_type(L, -2) == LUA_TSTRING) HasFields = true;
					else if (lua_type(L, -2) == LUA_TNUMBER) {
						const auto Key = lua_tonumber(L, -2);
						if (!(Key >= 1 && Key <= PersistenceMaxDocumentNodes) || std::trunc(Key) != Key)
							Invalid("[Persistence:Value] Array indices must be consecutive positive integers");
						Array = true;
						Last = std::max(Last, static_cast<std::size_t>(Key));
					} else Invalid("[Persistence:Value] Document keys must be strings or array indices");
					lua_pop(L, 2);
				}
				if (Array) {
					if (HasFields || Count != Last) Invalid("[Persistence:Value] Document arrays must be dense and contain only array indices");
					Value::Array Result(Count);
					for (int Cursor = lua_rawiter(L, Index, 0); Cursor >= 0; Cursor = lua_rawiter(L, Index, Cursor)) {
						Result[static_cast<std::size_t>(lua_tonumber(L, -2)) - 1] = Read(-1, Depth + 1);
						lua_pop(L, 2);
					}
					return {std::move(Result)};
				}
				Value::Object Result;
				Result.reserve(Count);
				for (int Cursor = lua_rawiter(L, Index, 0); Cursor >= 0; Cursor = lua_rawiter(L, Index, Cursor)) {
					auto Field = ReadString(L, -2, PersistenceMaxFieldBytes);
					if (!IsPersistenceField(Field)) Invalid("[Persistence:Value] Invalid document field name");
					Charge(Field.size() + 4);
					Result.push_back({std::move(Field), Read(-1, Depth + 1)});
					lua_pop(L, 2);
				}
				std::ranges::sort(Result, {}, &PersistenceDocumentField::Name);
				return {std::move(Result)};
			}
		};

		void PushDocument(lua_State* L, const Value& Document) {
			std::visit([&](const auto& Data) {
				using Type = std::decay_t<decltype(Data)>;
				if constexpr (std::is_same_v<Type, std::monostate>) lua_pushlightuserdata(L, &NullSentinel);
				else if constexpr (std::is_same_v<Type, bool>) lua_pushboolean(L, Data);
				else if constexpr (std::is_same_v<Type, double>) lua_pushnumber(L, Data);
				else if constexpr (std::is_same_v<Type, std::string>) PushString(L, Data);
				else if constexpr (std::is_same_v<Type, Value::Array>) {
					PushArrayTable(L, static_cast<int>(Data.size()));
					int Index = 1;
					for (const auto& Item : Data) {
						PushDocument(L, Item);
						lua_rawseti(L, -2, Index++);
					}
				} else {
					lua_createtable(L, 0, static_cast<int>(Data.size()));
					for (const auto& Field : Data) {
						PushString(L, Field.Name);
						PushDocument(L, Field.Value);
						lua_rawset(L, -3);
					}
				}
			}, Document.Data);
		}

		void PushRecord(lua_State* L, const PersistenceRecord& Record) {
			lua_createtable(L, 0, 3);
			PushString(L, Record.Key);
			lua_setfield(L, -2, "Key");
			PushString(L, std::to_string(Record.Revision));
			lua_setfield(L, -2, "Revision");
			if (const auto* Bytes = std::get_if<PersistenceBytes>(&Record.Payload)) {
				auto* Destination = lua_newbuffer(L, Bytes->size());
				if (!Bytes->empty()) std::memcpy(Destination, Bytes->data(), Bytes->size());
			} else PushDocument(L, std::get<Value>(Record.Payload));
			lua_setfield(L, -2, "Value");
		}

		const Value::Object& ReadObject(const Value& Data) {
			const auto* Object = std::get_if<Value::Object>(&Data.Data);
			if (!Object) Invalid("[Persistence:Query] Expected a dictionary");
			return *Object;
		}

		const Value::Array& ReadArray(const Value& Data, std::size_t Maximum) {
			const auto* Array = std::get_if<Value::Array>(&Data.Data);
			if (!Array || Array->size() > Maximum) Invalid("[Persistence:Query] Expected a bounded array");
			return *Array;
		}

		const std::string& ReadString(const Value& Data) {
			const auto* Text = std::get_if<std::string>(&Data.Data);
			if (!Text) Invalid("[Persistence:Query] Expected a string");
			return *Text;
		}

		std::vector<std::string> ReadPath(const Value& Data) {
			std::vector<std::string> Path;
			for (const auto& Part : ReadArray(Data, PersistenceMaxPathDepth)) Path.push_back(ReadString(Part));
			return Path;
		}

		PersistenceComparison ReadComparison(std::string_view Name) {
			using Comparison = PersistenceComparison;
			constexpr std::array Operators{
				std::pair{"Equal", Comparison::Equal}, std::pair{"NotEqual", Comparison::NotEqual},
				std::pair{"Less", Comparison::Less}, std::pair{"LessOrEqual", Comparison::LessOrEqual},
				std::pair{"Greater", Comparison::Greater}, std::pair{"GreaterOrEqual", Comparison::GreaterOrEqual},
				std::pair{"Exists", Comparison::Exists},
			};
			for (const auto& [Label, Operator] : Operators) if (Name == Label) return Operator;
			Invalid("[Persistence:Query] Unknown comparison");
		}

		PersistenceFilter ReadFilter(const Value& Data) {
			PersistenceFilter Filter;
			bool HasOperand = false;
			bool HasOperator = false;
			for (const auto& [Name, Field] : ReadObject(Data)) {
				if (Name == "Path") Filter.Path = ReadPath(Field);
				else if (Name == "Operator") {
					Filter.Operator = ReadComparison(ReadString(Field));
					HasOperator = true;
				} else if (Name == "Value") {
					Filter.Value = Field;
					HasOperand = true;
				} else Invalid("[Persistence:Query] Unexpected filter field");
			}
			if (!HasOperator || HasOperand == (Filter.Operator == PersistenceComparison::Exists))
				Invalid("[Persistence:Query] Comparison has an invalid operand");
			return Filter;
		}

		PersistenceOrder ReadOrder(const Value& Data) {
			PersistenceOrder Order;
			for (const auto& [Name, Field] : ReadObject(Data)) {
				if (Name == "Path") Order.Path = ReadPath(Field);
				else if (Name == "Descending") {
					const auto* Flag = std::get_if<bool>(&Field.Data);
					if (!Flag) Invalid("[Persistence:Query] Descending must be boolean");
					Order.Descending = *Flag;
				} else Invalid("[Persistence:Query] Unexpected order field");
			}
			return Order;
		}

		PersistenceQuery ReadQuery(lua_State* L, int Index) {
			if (lua_isnoneornil(L, Index)) return {};
			const auto Options = DocumentReader(L).Read(Index);
			PersistenceQuery Query;
			for (const auto& [Name, Field] : ReadObject(Options)) {
				if (Name == "Filters") {
					for (const auto& Filter : ReadArray(Field, PersistenceMaxFilters)) Query.Filters.push_back(ReadFilter(Filter));
				} else if (Name == "Order") {
					for (const auto& Order : ReadArray(Field, PersistenceMaxOrder)) Query.Order.push_back(ReadOrder(Order));
				} else if (Name == "Cursor") Query.Cursor = ReadString(Field);
				else if (Name == "Limit") {
					const auto* Number = std::get_if<double>(&Field.Data);
					if (!Number || *Number < 1 || *Number > PersistenceMaxPageRecords || std::trunc(*Number) != *Number)
						Invalid("[Persistence:Query] Limit must be a bounded positive integer");
					Query.Limit = static_cast<std::uint32_t>(*Number);
				} else Invalid("[Persistence:Query] Unexpected query field");
			}
			return Query;
		}

		std::optional<std::uint64_t> ReadRevision(lua_State* L, int Index) {
			if (lua_isnoneornil(L, Index)) return std::nullopt;
			const auto Text = ReadStringView(L, Index);
			if (Text.empty() || Text.size() > 20 || (Text.size() > 1 && Text.front() == '0'))
				Invalid("[Persistence:Revision] Expected a canonical unsigned decimal revision");
			std::uint64_t Revision = 0;
			const auto End = Text.data() + Text.size();
			const auto [Rest, Error] = std::from_chars(Text.data(), End, Revision);
			if (Error != std::errc{} || Rest != End)
				Invalid("[Persistence:Revision] Expected a canonical unsigned decimal revision");
			return Revision;
		}

		std::chrono::milliseconds ReadTimeout(lua_State* L, int Index) {
			if (lua_isnoneornil(L, Index)) return PersistenceRuntime::DefaultDeadline;
			if (lua_type(L, Index) != LUA_TNUMBER) Invalid("[Persistence:Deadline] Timeout must be milliseconds");
			const auto Timeout = lua_tonumber(L, Index);
			if (!(Timeout >= 1 && Timeout <= PersistenceRuntime::MaximumDeadline.count()) || std::trunc(Timeout) != Timeout)
				Invalid("[Persistence:Deadline] Timeout outside the supported range");
			return std::chrono::milliseconds(static_cast<int>(Timeout));
		}
	}

	G_USERDATA_DECL(DataStoreOperationHandle,
		std::weak_ptr<PersistenceRuntime> Runtime;
		std::shared_ptr<PersistenceHandle> Handle;
	);

	G_USERDATA_DECL(DataStoreHandle,
		std::weak_ptr<PersistenceRuntime> Runtime;
		PersistenceStoreKind Kind;
		std::string Name;
	);

	namespace {
		int Poll(lua_State* L, DataStoreOperationHandle* Operation) {
			runtime_detail::WorkScope Measure(runtime_detail::WorkPhase::PersistencePoll);
			(void)RequireRuntime(L, Operation->Runtime);
			if (!lua_checkstack(L, static_cast<int>(4 * PersistenceMaxDocumentDepth)))
				throw std::runtime_error("[Persistence:Value] Luau stack limit reached");
			const auto& Result = Operation->Handle->Poll();
			lua_createtable(L, 0, 4);
			lua_pushstring(L, GetPersistenceStatusName(Result.Status));
			lua_setfield(L, -2, "Status");
			if (Result.Revision) {
				PushString(L, std::to_string(Result.Revision));
				lua_setfield(L, -2, "Revision");
			}
			if (Result.Record) {
				PushRecord(L, *Result.Record);
				lua_setfield(L, -2, "Record");
			}
			if (Result.Status == PersistenceStatus::Ok && !Result.Revision && !Result.Record) {
				lua_createtable(L, static_cast<int>(Result.Records.size()), 0);
				int Index = 1;
				for (const auto& Record : Result.Records) {
					PushRecord(L, Record);
					lua_rawseti(L, -2, Index++);
				}
				lua_setfield(L, -2, "Records");
				if (!Result.Cursor.empty()) {
					PushString(L, Result.Cursor);
					lua_setfield(L, -2, "Cursor");
				}
			}
			Operation->Handle->ConsumePayload();
			return 1;
		}

		int Cancel(lua_State* L, DataStoreOperationHandle* Operation) {
			(void)RequireRuntime(L, Operation->Runtime);
			lua_pushboolean(L, Operation->Handle->Cancel());
			return 1;
		}

		int Capabilities(lua_State* L, DataStoreHandle* Store) {
			const auto Capabilities = RequireRuntime(L, Store->Runtime)->Capabilities();
			lua_createtable(L, 0, 3);
			lua_pushboolean(L, Capabilities.KeyValue);
			lua_setfield(L, -2, "KeyValue");
			lua_pushboolean(L, Capabilities.Documents);
			lua_setfield(L, -2, "Documents");
			lua_pushboolean(L, Capabilities.DocumentQueries);
			lua_setfield(L, -2, "DocumentQueries");
			return 1;
		}

		template <PersistenceOperation Operation>
		int Submit(lua_State* L, DataStoreHandle* Store) {
			runtime_detail::WorkScope Measure(runtime_detail::WorkPhase::PersistenceSubmit);
			auto Runtime = RequireRuntime(L, Store->Runtime);
			PersistenceRequest Request{.Kind = Store->Kind, .Operation = Operation, .Store = Store->Name};
			int TimeoutIndex = 3;
			if constexpr (Operation == PersistenceOperation::Query) {
				Request.Query = ReadQuery(L, 2);
			} else {
				Request.Key = ReadString(L, 2, PersistenceMaxKeyBytes);
				if constexpr (Operation == PersistenceOperation::Put) {
					if (Store->Kind == PersistenceStoreKind::KeyValue) {
						if (lua_type(L, 3) != LUA_TBUFFER) Invalid("[Persistence:Value] Key/value stores require a buffer");
						std::size_t Size = 0;
						const auto* Bytes = static_cast<const std::byte*>(lua_tobuffer(L, 3, &Size));
						if (Size > PersistenceMaxValueBytes) Invalid("[Persistence:Value] Buffer exceeds its byte limit");
						Request.Payload = Size == 0 ? PersistenceBytes{} : PersistenceBytes(Bytes, Bytes + Size);
					} else Request.Payload = DocumentReader(L).Read(3);
					Request.ExpectedRevision = ReadRevision(L, 4);
					TimeoutIndex = 5;
				}
				if constexpr (Operation == PersistenceOperation::Delete) {
					Request.ExpectedRevision = ReadRevision(L, 3);
					TimeoutIndex = 4;
				}
			}
			const auto Timeout = ReadTimeout(L, TimeoutIndex);
			auto Owner = ScriptEngine::Get(L)->FindManagedScriptThread(L);
			// Root the operation before native dispatch. Filling it performs no VM allocation.
			StackValue<DataStoreOperationHandle>::Push(L, { {}, Runtime, {} });
			auto* OnStack = static_cast<DataStoreOperationHandle*>(
				lua_touserdatatagged(L, -1, static_cast<int>(UserdataTag::PersistenceOperation)));
			OnStack->Handle = Runtime->Submit(std::move(Request), std::move(Owner), Timeout);
			return 1;
		}

		int GetStore(lua_State* L, Instance* Service, PersistenceStoreKind Kind) {
			auto Runtime = RequireRuntime(L, Service);
			auto Name = ReadString(L, 2, PersistenceMaxStoreBytes);
			if (!IsPersistenceStoreName(Name)) Invalid("[Persistence:Store] Invalid store name");
			return StackValue<DataStoreHandle>::Push(L, { {}, Runtime, Kind, std::move(Name) });
		}
	}

	G_USERDATA_IMPL(DataStoreOperationHandle,
		.Tag = UserdataTag::PersistenceOperation,
		.Type = "DataStoreOperation",
		.Methods = {{"Poll", {.Call = &Poll}}, {"Cancel", {.Call = &Cancel}}}
	);

	G_USERDATA_IMPL(DataStoreHandle,
		.Tag = UserdataTag::DataStore,
		.Type = "DataStore",
		.Methods = {
			{"GetAsync", {.Call = &Submit<PersistenceOperation::Get>}},
			{"PutAsync", {.Call = &Submit<PersistenceOperation::Put>}},
			{"DeleteAsync", {.Call = &Submit<PersistenceOperation::Delete>}},
			{"QueryAsync", {.Call = &Submit<PersistenceOperation::Query>}},
			{"GetCapabilities", {.Call = &Capabilities}},
		}
	);

	int DataStoreService::GetKeyValueStore(lua_State* L, Instance* Service) {
		return GetStore(L, Service, PersistenceStoreKind::KeyValue);
	}

	int DataStoreService::GetDocumentStore(lua_State* L, Instance* Service) {
		return GetStore(L, Service, PersistenceStoreKind::Document);
	}

	int DataStoreService::GetNull(lua_State* L, Instance* Service) {
		(void)RequireRuntime(L, Service);
		lua_pushlightuserdata(L, &NullSentinel);
		return 1;
	}

	int DataStoreService::CreateArray(lua_State* L, Instance* Service) {
		(void)RequireRuntime(L, Service);
		PushArrayTable(L, 0);
		return 1;
	}

	void RegisterDataStoreUserdata(lua_State* L) {
		DataStoreHandle::CreateUserdataMetatable(L);
		DataStoreOperationHandle::CreateUserdataMetatable(L);
		lua_createtable(L, 0, 1);
		lua_pushstring(L, "DataStoreArray");
		lua_setfield(L, -2, "__metatable");
		lua_setreadonly(L, -1, true);
		lua_setfield(L, LUA_REGISTRYINDEX, ArrayMetatable);
	}
}
