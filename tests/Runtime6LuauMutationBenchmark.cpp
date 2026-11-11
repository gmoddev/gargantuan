#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	struct TeardownMarker {
		const char *Phase;
		~TeardownMarker() { std::cerr << "[Runtime:LuauMutation] teardown=" << Phase << '\n' << std::flush; }
	};

	struct Options {
		std::size_t Count = 1000;
		std::size_t Frames = 12;
		std::size_t Listeners = 2;
		std::string Property = "CFrame";
	};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::size_t ParseBounded(std::string_view Text, std::size_t Maximum) {
		std::size_t Result = 0;
		const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Result);
		if (Error != std::errc{} || End != Text.data() + Text.size() || Result == 0 || Result > Maximum)
			throw std::invalid_argument("count/frames must be positive bounded integers");
		return Result;
	}

	Options Parse(int ArgumentCount, char **Arguments) {
		Options Config;
		bool SawCount = false, SawFrames = false, SawListeners = false, SawProperty = false;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			if (Argument.starts_with("--count=") && !SawCount) {
				Config.Count = ParseBounded(Argument.substr(8), 10000);
				SawCount = true;
			} else if (Argument.starts_with("--frames=") && !SawFrames) {
				Config.Frames = ParseBounded(Argument.substr(9), 12);
				SawFrames = true;
			} else if (Argument.starts_with("--listeners=") && !SawListeners) {
				const auto Value = Argument.substr(12);
				if (Value != "0" && Value != "2") throw std::invalid_argument("listeners must be 0 or 2");
				Config.Listeners = Value == "2" ? 2 : 0;
				SawListeners = true;
			} else if (Argument.starts_with("--property=") && !SawProperty) {
				const auto Value = Argument.substr(11);
				if (Value != "cframe" && Value != "color") throw std::invalid_argument("property must be cframe or color");
				Config.Property = Value == "cframe" ? "CFrame" : "Color";
				SawProperty = true;
			} else throw std::invalid_argument("unknown or duplicate benchmark option");
		}
		return Config;
	}

	WireValue ExpectedValue(const Options &Config, std::size_t Index, std::size_t Frame) {
		if (Config.Property == "CFrame") return WireCFrame{{
			static_cast<float>(Index), 0.0f, static_cast<float>(Frame),
			1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f
		}};
		return WireColor3{static_cast<float>(Frame) / 16.0f, 0.25f, 0.5f};
	}

	void ExportScriptFunction(lua_State *Source, lua_State *Destination, const char *Name) {
		lua_getglobal(Source, Name);
		Require(lua_isfunction(Source, -1), "server Script did not export its benchmark function");
		lua_xmove(Source, Destination, 1);
		lua_setglobal(Destination, Name);
	}

	void Call(lua_State *State, const char *Name, int Arguments, int Results,
		const ScriptSecurityContext &Security) {
		const int Base = lua_gettop(State) - Arguments;
		lua_getglobal(State, Name);
		Require(lua_isfunction(State, -1), "benchmark function is absent");
		lua_insert(State, Base + 1);
		ScriptSecurityScope Scope(Security);
		if (lua_pcall(State, Arguments, Results, 0) != LUA_OK) {
			const char *Error = lua_tostring(State, -1);
			throw std::runtime_error(Error ? Error : "unknown Luau execution error");
		}
	}

	// The source is run by a real server Script. It caches one set of existing
	// Parts and uses ordinary reflected property writes for each measured frame.
	constexpr std::string_view Source = R"LUA(
		local Folder = game.Workspace:FindFirstChild("MutationParts")
		if Folder == nil then error("mutation Parts folder is missing") end
		local Parts = Folder:GetChildren()
		local Property = BenchmarkProperty
		local Listeners = BenchmarkListeners
		local Count = #Parts
		local FirstCount, SecondCount, Failures = 0, 0, 0
		local CurrentFrame = 0
		local Tracking = true
		local Connections = {}
		for Index, Part in ipairs(Parts) do
			if Part.Name ~= "MutationPart" .. tostring(Index) then
				error("Part traversal order differs from fixture order")
			end
			if Listeners == 2 then
				local Pending = false
				local Signal = Part:GetPropertyChangedSignal(Property)
				Connections[#Connections + 1] = Signal:Connect(function()
					if not Tracking then return end
					local Expected = (CurrentFrame - 1) * Count + Index
					if Pending or FirstCount ~= Expected - 1 or SecondCount ~= Expected - 1 then Failures += 1 end
					Pending = true
					FirstCount += 1
					if Property == "CFrame" then
						if Part.CFrame.Position.Z ~= CurrentFrame then Failures += 1 end
					else
						if Part.Color.R ~= CurrentFrame / 16 then Failures += 1 end
					end
				end)
				Connections[#Connections + 1] = Signal:Connect(function()
					if not Tracking then return end
					local Expected = (CurrentFrame - 1) * Count + Index
					if not Pending or FirstCount ~= Expected or SecondCount ~= Expected - 1 then Failures += 1 end
					Pending = false
					SecondCount += 1
				end)
			end
		end
		function RunMutationFrame(Frame)
			CurrentFrame = Frame
			if #Connections ~= Count * Listeners then Failures += 1 end
			if Property == "CFrame" then
				for Index, Part in ipairs(Parts) do Part.CFrame = CFrame.new(Index, 0, Frame) end
			else
				for _, Part in ipairs(Parts) do Part.Color = Color3.new(Frame / 16, 0.25, 0.5) end
			end
			return FirstCount, SecondCount, Failures
		end
		function AttemptDeniedMutation()
			return pcall(function() Parts[1].CFrame = CFrame.new(999, 0, 0) end)
		end
		function AttemptInvalidMutation()
			return pcall(function() Parts[1][Property] = "invalid" end)
		end
		function AttemptReadOnlyMutation()
			return pcall(function() Parts[1].Destroyed = true end)
		end
		function RunReentrantMutation()
			Tracking = false
			local Part = Parts[1]
			local Order = {}
			local CFrameConnection = Part:GetPropertyChangedSignal("CFrame"):Connect(function()
				Order[#Order + 1] = "CFrame"
				if Part.CFrame.Position.Z ~= 13 then Failures += 1 end
				Part.Color = Color3.new(0.875, 0.25, 0.5)
			end)
			local ColorConnection = Part:GetPropertyChangedSignal("Color"):Connect(function()
				Order[#Order + 1] = "Color"
			end)
			Part.CFrame = CFrame.new(1, 0, 13)
			CFrameConnection:Disconnect()
			ColorConnection:Disconnect()
			return table.concat(Order, ","), Failures
		end
	)LUA";
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const auto Config = Parse(ArgumentCount, Arguments);
		BootstrapNativeRuntimeSchema();
		TeardownMarker AfterGame{"after-game"};
		auto Game = std::make_shared<DataModel>();
		auto World = Game->GetService("Workspace");
		auto FolderValue = std::make_shared<Folder>();
		FolderValue->SetName("MutationParts");
		FolderValue->SetParent(World);
		TeardownMarker AfterParts{"after-parts"};
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Config.Count);
		for (std::size_t Index = 1; Index <= Config.Count; ++Index) {
			auto Value = std::make_shared<Part>();
			Value->SetName("MutationPart" + std::to_string(Index));
			Value->SetAnchored(true);
			Value->SetParent(FolderValue);
			Parts.push_back(std::move(Value));
		}
		std::vector<std::string> Diagnostics;
		TeardownMarker AfterEngine{"after-engine"};
		ScriptEngine Engine(Game, [&](std::string Severity, std::string Message) {
			if (Severity == "Error") Diagnostics.push_back(std::move(Message));
		});
		lua_pushstring(Engine.L, Config.Property.c_str());
		lua_setglobal(Engine.L, "BenchmarkProperty");
		lua_pushinteger(Engine.L, static_cast<lua_Integer>(Config.Listeners));
		lua_setglobal(Engine.L, "BenchmarkListeners");
		TeardownMarker AfterScript{"after-script"};
		auto ScriptValue = std::make_shared<Script>();
		ScriptValue->SetName("MutationHarness");
		ScriptValue->SetSource(std::string(Source));
		ScriptValue->SetParent(World);
		Engine.RunBootstrapScript(ScriptValue);
		Require(ScriptValue->Status == ScriptStatus::Finished && ScriptValue->Thread && Diagnostics.empty(),
			"server Script setup failed");
		for (const char *Name : {"RunMutationFrame", "AttemptDeniedMutation", "AttemptInvalidMutation",
			"AttemptReadOnlyMutation", "RunReentrantMutation"})
			ExportScriptFunction(ScriptValue->Thread, Engine.L, Name);
		// Connection userdata disconnects on GC. Prove the Script retains both
		// callbacks per Part before the timed loop starts.
		lua_gc(Engine.L, LUA_GCCOLLECT, 0);
		const auto Scope = Game->GetObjectId();
		auto &Journal = ChangeJournal::Get();
		std::cout << "property,count,frame,listeners,vm_ms,journal_records,first_callbacks,second_callbacks,event_failures,first_sequence,last_sequence\n";
		for (std::size_t Frame = 1; Frame <= Config.Frames; ++Frame) {
			const auto Cursor = Journal.CreateCursor(Scope);
			lua_pushinteger(Engine.L, static_cast<lua_Integer>(Frame));
			const auto Started = Clock::now();
			Call(Engine.L, "RunMutationFrame", 1, 3, ScriptSecurityContext::ServerRuntime());
			const auto VmMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
			const auto First = static_cast<std::uint64_t>(lua_tointeger(Engine.L, -3));
			const auto Second = static_cast<std::uint64_t>(lua_tointeger(Engine.L, -2));
			const auto Failures = static_cast<std::uint64_t>(lua_tointeger(Engine.L, -1));
			lua_settop(Engine.L, 0);
			const auto ExpectedCallbacks = Config.Listeners ? Frame * Config.Count : 0;
			Require(First == ExpectedCallbacks && Second == ExpectedCallbacks && Failures == 0,
				"per-property Luau callback count/order/immediate value differs");
			const auto Read = Journal.Read(Cursor, Config.Count + 1);
			Require(Read.Status == ChangeReadStatus::Available && Read.Records.size() == Config.Count &&
				Read.Cursor.NextSequence == Cursor.NextSequence + Config.Count,
				"script writes did not append exactly one ordered record each");
			for (std::size_t Index = 0; Index < Config.Count; ++Index) {
				const auto &Record = Read.Records[Index];
				const auto *Property = std::get_if<PropertyUpdatedChange>(&Record.Payload);
				const auto Expected = ExpectedValue(Config, Index + 1, Frame);
				Require(Record.Sequence == Cursor.NextSequence + Index && Record.Scope == Scope &&
					Record.Object == Parts[Index]->GetObjectId() && Property &&
					Property->PropertyName == Config.Property && Property->Value == Expected &&
					Parts[Index]->ReadPropertyWireValue(Config.Property) == std::optional<WireValue>(Expected),
					"script journal/order/final authoritative state differs");
			}
			Require(Journal.CreateCursor(Scope).NextSequence == Read.Cursor.NextSequence,
				"script frame emitted extra committed records");
			std::cout << Config.Property << ',' << Config.Count << ',' << Frame << ',' << Config.Listeners << ','
				<< std::fixed << std::setprecision(6) << VmMilliseconds << ',' << Read.Records.size() << ','
				<< First << ',' << Second << ',' << Failures << ',' << Cursor.NextSequence << ','
				<< Read.Cursor.NextSequence - 1 << '\n';
		}
		const auto BeforeRejected = Journal.CreateCursor(Scope);
		const auto BeforeValue = Parts.front()->ReadPropertyWireValue(Config.Property);
		const auto BeforeCFrame = Parts.front()->ReadPropertyWireValue("CFrame");
		std::size_t RejectedSignalCount = 0;
		TeardownMarker AfterConnections{"after-native-connections"};
		auto CFrameConnection = Parts.front()->GetPropertyChangedSignal("CFrame")->Connect(
			[&](std::monostate) { ++RejectedSignalCount; });
		auto ColorConnection = Parts.front()->GetPropertyChangedSignal("Color")->Connect(
			[&](std::monostate) { ++RejectedSignalCount; });
		{
			ScriptSecurityScope Restricted({ScriptExecutionDomain::Server, {ScriptCapability::ReadDataModel}});
			Call(Engine.L, "AttemptDeniedMutation", 0, 1, GetCurrentScriptSecurityContext());
			Require(!lua_toboolean(Engine.L, -1), "read-only Luau context unexpectedly mutated a Part");
			lua_settop(Engine.L, 0);
		}
		for (const char *Name : {"AttemptInvalidMutation", "AttemptReadOnlyMutation"}) {
			Call(Engine.L, Name, 0, 1, ScriptSecurityContext::ServerRuntime());
			Require(!lua_toboolean(Engine.L, -1), "invalid/read-only Luau write unexpectedly succeeded");
			lua_settop(Engine.L, 0);
		}
		Require(Journal.Read(BeforeRejected).Records.empty() &&
			Parts.front()->ReadPropertyWireValue(Config.Property) == BeforeValue &&
			Parts.front()->ReadPropertyWireValue("CFrame") == BeforeCFrame &&
			!Parts.front()->GetDestroyed() && RejectedSignalCount == 0,
			"rejected Luau write changed journal, state, or signals");
		const auto BeforeNested = Journal.CreateCursor(Scope);
		Call(Engine.L, "RunReentrantMutation", 0, 2, ScriptSecurityContext::ServerRuntime());
		const char *Order = lua_tostring(Engine.L, -2);
		Require(Order && std::string_view(Order) == "CFrame,Color" && lua_tointeger(Engine.L, -1) == 0,
			"nested per-property callbacks lost synchronous event order");
		lua_settop(Engine.L, 0);
		const auto Nested = Journal.Read(BeforeNested, 3);
		const WireValue NestedCFrame = WireCFrame{{1.0f, 0.0f, 13.0f,
			1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
		const WireValue NestedColor = WireColor3{0.875f, 0.25f, 0.5f};
		Require(Nested.Records.size() == 2 && Nested.Records[0].Object == Parts.front()->GetObjectId() &&
			Nested.Records[1].Object == Parts.front()->GetObjectId() &&
			Nested.Records[0].Scope == Scope && Nested.Records[1].Scope == Scope &&
			Nested.Records[0].Sequence == BeforeNested.NextSequence &&
			Nested.Records[1].Sequence == BeforeNested.NextSequence + 1 &&
			std::get_if<PropertyUpdatedChange>(&Nested.Records[0].Payload) &&
			std::get<PropertyUpdatedChange>(Nested.Records[0].Payload).PropertyName == "CFrame" &&
			std::get<PropertyUpdatedChange>(Nested.Records[0].Payload).Value == NestedCFrame &&
			std::get_if<PropertyUpdatedChange>(&Nested.Records[1].Payload) &&
			std::get<PropertyUpdatedChange>(Nested.Records[1].Payload).PropertyName == "Color" &&
			std::get<PropertyUpdatedChange>(Nested.Records[1].Payload).Value == NestedColor &&
			Parts.front()->ReadPropertyWireValue("CFrame") == std::optional<WireValue>(NestedCFrame) &&
			Parts.front()->ReadPropertyWireValue("Color") == std::optional<WireValue>(NestedColor) &&
			RejectedSignalCount == 2 &&
			Journal.CreateCursor(Scope).NextSequence == BeforeNested.NextSequence + 2,
			"nested script mutation did not append CFrame then Color exactly once");
		std::cerr << "[Runtime:LuauMutation] validation=pass denied=1 invalid=1 readonly=1 reentrant=1\n";
		std::cerr << "[Runtime:LuauMutation] teardown=before-final-gc\n" << std::flush;
		lua_gc(Engine.L, LUA_GCCOLLECT, 0);
		std::cerr << "[Runtime:LuauMutation] teardown=after-final-gc\n" << std::flush;
		TeardownMarker BeginTeardown{"begin-locals"};
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Runtime:LuauMutation] " << Error.what() << '\n';
		return 1;
	}
}
