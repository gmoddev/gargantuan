#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <Luau/Compiler.h>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	struct RecordingScheduler final : INetworkScheduler {
		std::vector<NetworkMessageIntent> Messages;
		bool RegisterConnection(ConnectionId, const NetworkLimits &) override {
			return true;
		}
		SchedulerSubmitResult Submit(NetworkMessageIntent Intent) override {
			Messages.push_back(std::move(Intent));
			return {SchedulerSubmitStatus::Accepted};
		}
		SchedulerFlushResult Flush(ConnectionId, SchedulerTickBudget) override {
			return {SchedulerFlushStatus::Drained};
		}
		bool CancelConnection(ConnectionId) override {
			return true;
		}
		std::optional<SchedulerStatistics> GetStatistics(ConnectionId) const override {
			return std::nullopt;
		}
	};

	NetworkLimits Limits() {
		return {
			.MaximumReliableMessageBytes = MaximumRemoteFrameBytes,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 1024 * 1024,
			.MaximumInFlightRemoteRequests = 16,
			.MaximumDecodedMessageBytes = MaximumRemoteFrameBytes,
			.MaximumSendBytesPerTick = 1024 * 1024,
			.MaximumReceiveBytesPerTick = 1024 * 1024,
			.MaximumMessagesPerTick = 1024,
		};
	}

	int Run(ScriptEngine &Engine, std::string_view Source, int Results = 0) {
		size_t BytecodeSize = 0;
		char *Bytecode = luau_compile(Source.data(), Source.size(), &Engine.CompileOptions, &BytecodeSize);
		if (!Bytecode) return LUA_ERRSYNTAX;
		const int Loaded = luau_load(Engine.L, "remote-luau-test", Bytecode, BytecodeSize, 0);
		std::free(Bytecode);
		if (Loaded != LUA_OK) return Loaded;
		return lua_pcall(Engine.L, 0, Results, 0);
	}

	void PushGlobal(lua_State *L, const char *Name, const std::shared_ptr<Instance> &Value) {
		StackValue<std::shared_ptr<Instance>>::Push(L, Value);
		lua_setglobal(L, Name);
	}

	ReceivedMessageEvent Incoming(ConnectionId Connection, const RemoteMessage &Message) {
		auto Bytes = EncodeRemoteMessage(Message);
		if (!Bytes) std::abort();
		return {Connection, DeliveryMode::ReliableOrdered, TrafficClass::ReliableApplication, {}, std::move(*Bytes)};
	}

	int HasMutateDataModel(lua_State *L) {
		lua_pushboolean(L, GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel));
		return 1;
	}
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	try {
		BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Error) {
		std::cerr << Error.what() << '\n';
		return 1;
	}

	auto Game = std::make_shared<DataModel>();
	ScriptEngine Engine(Game);
	lua_pushcfunction(Engine.L, HasMutateDataModel, "HasMutateDataModel");
	lua_setglobal(Engine.L, "HasMutateDataModel");
	RecordingScheduler Scheduler;
	const ConnectionId Connection{7, 3};
	std::set<ObjectId> Visible;
	auto Time = std::chrono::steady_clock::time_point{};
	RemoteManager Client(
		RemoteManagerRole::Client,
		Scheduler,
		[&](ConnectionId, ObjectId Object) { return Visible.contains(Object); },
		[](ObjectId Object) { return ObjectRegistry::Get().Lookup(Object); },
		[&] { return Time; }
	);
	Check(Client.AddPeer(Connection, ReplicationEpoch(9), Limits()), "Luau client RemoteManager adds its server peer");

	auto Event = std::make_shared<RemoteEvent>();
	Visible.insert(Event->GetObjectId());
	Check(Event->BindRemoteManager(&Client, Connection), "RemoteEvent binds through the Instance-native boundary");
	Check(Client.PublishRemote(Connection, Event->GetObjectId()), "RemoteEvent is published to the Luau client peer");
	PushGlobal(Engine.L, "Remote", Event);
	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine, "Remote:FireServer('hello', 7, Vector3.new(1, 2, 3))") == LUA_OK,
			"Luau RemoteEvent:FireServer accepts supported bounded arguments"
		);
	}
	Check(Scheduler.Messages.size() == 1, "Luau FireServer submits one scheduler message without request tracking");
	if (!Scheduler.Messages.empty()) {
		auto Decoded = DecodeRemoteMessage(Scheduler.Messages.back().Payload());
		Check(
			Decoded && Decoded->Kind == RemoteMessageKind::ReliableEvent && Decoded->Arguments.size() == 3 &&
				std::get<std::string>(Decoded->Arguments[0]) == "hello" && std::get<int>(Decoded->Arguments[1]) == 7,
			"Luau arguments cross the typed Remote codec boundary"
		);
	}

	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Server,
			 {ScriptCapability::ReadDataModel, ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine, "Remote:FireServer('wrong direction')") != LUA_OK,
			"server runtime cannot call client-only FireServer"
		);
		lua_settop(Engine.L, 0);
	}
	{
		ScriptSecurityScope Scope(ScriptSecurityContext::PreRunRegistration());
		Check(
			Run(Engine, "Remote:FireServer('prerun')") != LUA_OK,
			"PreRun schema authority does not become game networking authority"
		);
		lua_settop(Engine.L, 0);
	}
	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine, "Remote:FireServer({unsafe = true})") != LUA_OK,
			"Luau tables and arbitrary container payloads are rejected"
		);
		lua_settop(Engine.L, 0);
		Check(
			Run(Engine, "Remote:FireServer(string.rep('x', 16385))") != LUA_OK,
			"Luau strings over 16 KiB are rejected before scheduler admission"
		);
		lua_settop(Engine.L, 0);
	}

	auto Function = std::make_shared<RemoteFunction>();
	Visible.insert(Function->GetObjectId());
	Check(
		Function->BindRemoteManager(&Client, Connection), "RemoteFunction binds through the Instance-native boundary"
	);
	Check(
		Client.PublishRemote(Connection, Function->GetObjectId()), "RemoteFunction is published to the Luau client peer"
	);
	PushGlobal(Engine.L, "Function", Function);
	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine, "return pcall(function() Function:InvokeServer() end)", 2) == LUA_OK &&
				!lua_toboolean(Engine.L, -2),
			"RemoteFunction cannot block or suspend the engine Main coroutine"
		);
		lua_settop(Engine.L, 0);
		Check(
			Run(Engine,
				R"(
			RequestResult = nil
			ResumedCanMutate = nil
			RequestThread = coroutine.create(function()
				RequestResult = Function:InvokeServerWithTimeout(1, 40)
				ResumedCanMutate = HasMutateDataModel()
			end)
			return coroutine.resume(RequestThread), coroutine.status(RequestThread)
		)",
				2) == LUA_OK &&
				lua_toboolean(Engine.L, -2) && std::string_view(lua_tostring(Engine.L, -1)) == "suspended",
			"RemoteFunction suspends only its calling Luau coroutine"
		);
		lua_settop(Engine.L, 0);
	}
	Check(!Scheduler.Messages.empty(), "suspended Luau RemoteFunction submits a request");
	auto Request = DecodeRemoteMessage(Scheduler.Messages.back().Payload());
	Check(
		Request && Request->Kind == RemoteMessageKind::Request && Request->Request.IsValid(),
		"Luau RemoteFunction uses an explicit correlated RequestId"
	);
	if (Request) {
		RemoteMessage Response{
			.Kind = RemoteMessageKind::Response,
			.Remote = Function->GetObjectId(),
			.Request = Request->Request,
			.Arguments = {42}
		};
		Check(
			Client.HandleTransportEvent(Incoming(Connection, Response)) && Client.Pump() == 1,
			"validated response reaches the Luau safe-point dispatcher"
		);
		lua_getglobal(Engine.L, "RequestResult");
		Check(lua_tointeger(Engine.L, -1) == 42, "successful response resumes the correct Luau coroutine once");
		lua_pop(Engine.L, 1);
		lua_getglobal(Engine.L, "ResumedCanMutate");
		Check(
			!lua_toboolean(Engine.L, -1),
			"RemoteFunction continuation preserves its original restricted capability context"
		);
		lua_pop(Engine.L, 1);
		Check(
			!Client.HandleTransportEvent(Incoming(Connection, Response)) || Client.Pump() == 1,
			"duplicate response is safely consumed or rejected without reactivation"
		);
	}

	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine,
				R"(
			BadResult, BadStatus = 'pending', 'pending'
			BadThread = coroutine.create(function()
				BadResult, BadStatus = Function:InvokeServerWithTimeout(1)
			end)
			return coroutine.resume(BadThread)
			)",
				1) == LUA_OK &&
				lua_toboolean(Engine.L, -1),
			"Luau request starts before a semantically unsupported response"
		);
		lua_settop(Engine.L, 0);
	}
	auto BadRequest = DecodeRemoteMessage(Scheduler.Messages.back().Payload());
	if (BadRequest) {
		RemoteMessage BadResponse{
			.Kind = RemoteMessageKind::Response,
			.Remote = Function->GetObjectId(),
			.Request = BadRequest->Request,
			.Arguments = {WireSchemaEnumValue{SchemaId::FromEnumName("Game", "Unavailable"), 1, 1}}
		};
		Check(
			Client.HandleTransportEvent(Incoming(Connection, BadResponse)) && Client.Pump() == 1,
			"protocol-valid but Luau-unavailable response reaches terminal conversion"
		);
		lua_getglobal(Engine.L, "BadStatus");
		Check(
			std::string_view(lua_tostring(Engine.L, -1)) == "protocol_rejected",
			"unsupported response resumes its caller with a bounded terminal error"
		);
		lua_pop(Engine.L, 1);
	}

	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine,
				R"(
			TimeoutValue, TimeoutStatus = 'pending', 'pending'
			TimeoutThread = coroutine.create(function()
				TimeoutValue, TimeoutStatus = Function:InvokeServerWithTimeout(0.01)
			end)
			return coroutine.resume(TimeoutThread)
		)",
				1) == LUA_OK &&
				lua_toboolean(Engine.L, -1),
			"finite-timeout Luau request starts asynchronously"
		);
		lua_settop(Engine.L, 0);
	}
	Time += 11ms;
	Client.Pump();
	lua_getglobal(Engine.L, "TimeoutStatus");
	Check(std::string_view(lua_tostring(Engine.L, -1)) == "timeout", "Luau timeout resumes with a terminal status");
	lua_pop(Engine.L, 1);

	RecordingScheduler ServerScheduler;
	RemoteManager Server(
		RemoteManagerRole::Server,
		ServerScheduler,
		[&](ConnectionId, ObjectId Object) { return Visible.contains(Object); },
		[](ObjectId Object) { return ObjectRegistry::Get().Lookup(Object); }
	);
	Check(Server.AddPeer(Connection, ReplicationEpoch(9), Limits()), "Luau server RemoteManager adds client peer");
	auto ServerEvent = std::make_shared<RemoteEvent>();
	Visible.insert(ServerEvent->GetObjectId());
	Check(
		ServerEvent->BindRemoteManager(&Server) && Server.PublishRemote(Connection, ServerEvent->GetObjectId()),
		"server RemoteEvent is bound and visible"
	);
	PushGlobal(Engine.L, "ServerRemote", ServerEvent);
	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Server,
			 {ScriptCapability::ReadDataModel, ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		const int ConnectStatus = Run(Engine, R"(
			SignalValue = 'unset'
			SignalConnection = ServerRemote.OnServerEvent:Connect(function(Peer, Text, Value)
				SignalValue = tostring(Peer.Slot) .. ':' .. Text .. ':' .. tostring(Value)
			end)
		)");
		if (ConnectStatus != LUA_OK) std::cerr << "Signal connect error: " << lua_tostring(Engine.L, -1) << '\n';
		Check(ConnectStatus == LUA_OK, "Luau connects to the server-side RemoteEvent signal");
	}
	RemoteMessage EventMessage{
		.Kind = RemoteMessageKind::ReliableEvent,
		.Remote = ServerEvent->GetObjectId(),
		.Arguments = {std::string("event"), 9}
	};
	auto UnsupportedEvent = EventMessage;
	UnsupportedEvent.Arguments = {WireSchemaEnumValue{SchemaId::FromEnumName("Game", "Unavailable"), 1, 1}};
	Check(
		Server.HandleTransportEvent(Incoming(Connection, UnsupportedEvent)) && Server.Pump() == 1,
		"unsupported Luau RemoteEvent value fails at the semantic handler boundary"
	);
	lua_getglobal(Engine.L, "SignalValue");
	Check(
		std::string_view(lua_tostring(Engine.L, -1)) == "unset",
		"unsupported RemoteEvent value never enters a gameplay callback"
	);
	lua_pop(Engine.L, 1);
	Check(
		Server.HandleTransportEvent(Incoming(Connection, EventMessage)) && Server.Pump() == 1,
		"incoming client event dispatches only after decoding and manager validation"
	);
	lua_getglobal(Engine.L, "SignalValue");
	Check(
		std::string_view(lua_tostring(Engine.L, -1)) == "7:event:9",
		"OnServerEvent delivers provisional immutable peer context and typed arguments"
	);
	lua_pop(Engine.L, 1);
	Check(Run(Engine, "SignalConnection:Disconnect()") == LUA_OK, "Luau RemoteEvent connection disconnects normally");
	EventMessage.Arguments = {std::string("ignored"), 10};
	Check(
		Server.HandleTransportEvent(Incoming(Connection, EventMessage)) && Server.Pump() == 1,
		"disconnected event remains protocol-valid"
	);
	lua_getglobal(Engine.L, "SignalValue");
	Check(
		std::string_view(lua_tostring(Engine.L, -1)) == "7:event:9", "disconnected Luau handler is not invoked again"
	);
	lua_pop(Engine.L, 1);

	ServerEvent->Destroy();
	Check(
		Server.SendEvent(Connection, ServerEvent->GetObjectId(), {}).Status == RemoteSendStatus::UnknownRemote,
		"destroyed Remote Instance cannot be used through native or Luau state"
	);

	if (Failures != 0) {
		std::cerr << Failures << " Luau Remote test(s) failed\n";
		return 1;
	}
	std::cout << "Luau RemoteEvent signals/directions/arguments and RemoteFunction coroutine lifecycle passed\n";
	return 0;
}
