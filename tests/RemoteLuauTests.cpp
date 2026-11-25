#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <Luau/Compiler.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
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

	void CheckLuau(ScriptEngine &Engine, std::string_view Source, const char *Message) {
		const int Base = lua_gettop(Engine.L);
		const int Status = Run(Engine, Source);
		if (Status != LUA_OK) {
			std::size_t Length = 0;
			const char *Error = lua_gettop(Engine.L) > Base ? lua_tolstring(Engine.L, -1, &Length) : nullptr;
			std::cerr << "[Networking:RemoteLuauTest] Luau status " << Status << ", "
				<< (Error ? std::string_view(Error, std::min<std::size_t>(Length, 512)) : "no error string") << '\n';
			lua_settop(Engine.L, Base);
		}
		Check(Status == LUA_OK, Message);
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
	auto CheckLifecycleMethods = [&](ScriptSecurityContext Context, bool Allowed,
		const char *ClearMessage, const char *DestroyMessage) {
		auto Parent = std::make_shared<Folder>();
		auto Child = std::make_shared<Folder>();
		Child->SetParent(Parent);
		PushGlobal(Engine.L, "LifecycleParent", Parent);
		{
			ScriptSecurityScope Scope(std::move(Context));
			const int ClearStatus = Run(Engine,
				"return pcall(function() LifecycleParent:ClearAllChildren() end)", 2);
			const bool ClearAllowed = ClearStatus == LUA_OK && lua_toboolean(Engine.L, -2);
			const auto *ClearError = ClearStatus == LUA_OK ? lua_tostring(Engine.L, -1) : nullptr;
			Check(ClearAllowed == Allowed && Child->GetDestroyed() == Allowed &&
				(Allowed || (ClearError && std::string_view(ClearError).find("cannot invoke method") != std::string_view::npos)),
				ClearMessage);
			lua_settop(Engine.L, 0);
			const int DestroyStatus = Run(Engine,
				"return pcall(function() LifecycleParent:Destroy() end)", 2);
			const bool DestroyAllowed = DestroyStatus == LUA_OK && lua_toboolean(Engine.L, -2);
			const auto *DestroyError = DestroyStatus == LUA_OK ? lua_tostring(Engine.L, -1) : nullptr;
			Check(DestroyAllowed == Allowed && Parent->GetDestroyed() == Allowed &&
				(Allowed || (DestroyError && std::string_view(DestroyError).find("cannot invoke method") != std::string_view::npos)),
				DestroyMessage);
			lua_settop(Engine.L, 0);
		}
		if (!Parent->GetDestroyed()) Parent->Destroy();
	};
	CheckLifecycleMethods(
		{ScriptExecutionDomain::Server, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}},
		false, "network-only server handler cannot clear children", "network-only server handler cannot destroy Instance"
	);
	CheckLifecycleMethods(
		{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}},
		false, "network-only client handler cannot clear children", "network-only client handler cannot destroy Instance"
	);
	CheckLifecycleMethods(ScriptSecurityContext::ServerRuntime(), true,
		"ordinary server runtime can clear children", "ordinary server runtime can destroy Instance");
	CheckLifecycleMethods(ScriptSecurityContext::ClientRuntime(), true,
		"ordinary client runtime can clear detached children", "ordinary client runtime can destroy detached Instance");
	lua_pushcfunction(Engine.L, HasMutateDataModel, "HasMutateDataModel");
	lua_setglobal(Engine.L, "HasMutateDataModel");
	RecordingScheduler Scheduler;
	const ConnectionId Connection{7, 3};
	std::set<ObjectId> Visible;
	auto Time = std::chrono::steady_clock::time_point{};
	const auto ReplicaReference = std::make_shared<Folder>();
	const ObjectId ReferenceSource{505050, 11};
	RemoteManager Client(
		RemoteManagerRole::Client,
		Scheduler,
		[&](ConnectionId, ObjectId Object) { return Visible.contains(Object); },
		[&](ObjectId Object) -> std::shared_ptr<Instance> {
			if (Object == ReferenceSource) return ReplicaReference;
			return ObjectRegistry::Get().Lookup(Object);
		},
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
		ScriptSecurityScope Scope(ScriptSecurityContext::ClientRuntime());
		Visible.insert(ReferenceSource);
		Check(Client.MarkMaterialized(Connection, ReferenceSource), "client reference fixture is published");
		Client.SetOutgoingReferenceMapper([&](const Instance &Object) -> std::optional<ObjectId> {
			if (&Object == ReplicaReference.get()) return ReferenceSource;
			return std::nullopt;
		});
		PushGlobal(Engine.L, "ReplicaReference", ReplicaReference);
		const auto Before = Scheduler.Messages.size();
		Check(Run(Engine, R"luau(
			local Blob = buffer.create(3)
			buffer.writeu8(Blob, 1, 255)
			local Item = {Reference = ReplicaReference, Text = "007"}
			local Packet = {Other = Item, Item = Item, Blob = Blob, Empty = {}}
			Remote:FireServer(Packet, "007", nil, buffer.create(0))
			buffer.writeu8(Blob, 1, 42)
			Item.Text = "changed"
		)luau") == LUA_OK && Scheduler.Messages.size() == Before + 1,
			"client snapshots nested values, a mapped Instance and buffers at submission");

		const std::vector<RemoteValue> Expected{
			RemoteDictionary{{
				{"Blob", RemoteBuffer{{std::byte{0}, std::byte{255}, std::byte{0}}}},
				{"Empty", RemoteArray{}},
				{"Item", RemoteDictionary{{
					{"Reference", WireObjectReference{WireObjectId::FromObjectId(ReferenceSource)}},
					{"Text", std::string("007")},
				}}},
				{"Other", RemoteDictionary{{
					{"Reference", WireObjectReference{WireObjectId::FromObjectId(ReferenceSource)}},
					{"Text", std::string("007")},
				}}},
			}},
			7, std::monostate{}, RemoteBuffer{},
		};
		if (Scheduler.Messages.size() == Before + 1) {
			const auto Decoded = DecodeRemoteMessage(Scheduler.Messages.back().Payload());
			Check(Decoded && Decoded->Arguments == Expected,
				"compound encoding preserves exact nested strings and snapshot values with legacy positional conversion");
		}
		Check(Run(Engine, R"luau(
			StructuredEvents = 0
			Remote.OnClientEvent:Connect(function(Packet, LegacyNumber, Absent, EmptyBuffer)
				assert(Packet.Item.Reference == ReplicaReference)
				assert(Packet.Other.Reference == ReplicaReference and Packet.Other ~= Packet.Item)
				assert(Packet.Item.Text == "007" and #Packet.Empty == 0)
				assert(LegacyNumber == 7 and Absent == nil and buffer.len(EmptyBuffer) == 0)
				assert(buffer.readu8(Packet.Blob, 1) == 255)
				buffer.writeu8(Packet.Blob, 1, 77)
				StructuredEvents += 1
			end)
			Remote.OnClientEvent:Connect(function(Packet)
				if buffer.readu8(Packet.Blob, 1) == 255 then StructuredEvents += 1 end
			end)
		)luau") == LUA_OK, "client connects two independent structured payload consumers");
		Check(Client.HandleTransportEvent(Incoming(Connection, {
			.Kind = RemoteMessageKind::ReliableEvent, .Remote = Event->GetObjectId(), .Arguments = Expected
		})) && Client.Pump() == 1,
			"nested references resolve by wire source identity on signal delivery");
		Check(Run(Engine, "return StructuredEvents", 1) == LUA_OK && lua_tointeger(Engine.L, -1) == 2,
			"receiver tables and native buffers are independent for each callback");
		lua_settop(Engine.L, 0);
		Client.SetOutgoingReferenceMapper({});
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
			Run(Engine, R"luau(
				local Cycle = {}
				Cycle.Child = Cycle
				local Bad = {
					Cycle, {[1] = 1, [3] = 3}, {1, Key = true}, {[true] = 1},
					setmetatable({}, {}), {buffer.create(65537)}, table.create(8193),
					{[string.rep("x", 257)] = true}, {{function() end}},
				}
				for _, Value in Bad do
					assert(not pcall(function() Remote:FireServer(Value) end))
				end
			)luau") == LUA_OK,
			"cycle, sparse/mixed/key rules, metatables, excessive storage and oversized buffers reject cleanly"
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
	auto RemoteSecurityTarget = std::make_shared<Folder>();
	PushGlobal(Engine.L, "RemoteSecurityTarget", RemoteSecurityTarget);
	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Server,
			 {ScriptCapability::ReadDataModel, ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		const int ConnectStatus = Run(Engine, R"(
			SignalValue = 'unset'
			DeferredCanMutate = nil
			DelayedCanMutate = nil
			WaitCanMutate = nil
			DeferredMutationOk = nil
			SignalConnection = ServerRemote.OnServerEvent:Connect(function(Peer, Text, Value)
				SignalValue = tostring(Peer.Slot) .. ':' .. Text .. ':' .. tostring(Value)
				task.defer(function()
					DeferredCanMutate = HasMutateDataModel()
					DeferredMutationOk = pcall(function()
						RemoteSecurityTarget:SetAttribute('Compromised', true)
					end)
				end)
				task.delay(0, function() DelayedCanMutate = HasMutateDataModel() end)
				task.spawn(function()
					task.wait(0)
					WaitCanMutate = HasMutateDataModel()
				end)
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
	Engine.Threads.Step();
	Engine.Threads.Step();
	for (const char *Name : {"DeferredCanMutate", "DelayedCanMutate", "WaitCanMutate", "DeferredMutationOk"}) {
		lua_getglobal(Engine.L, Name);
		Check(!lua_toboolean(Engine.L, -1), "Remote-triggered task work retains network-only authority");
		lua_pop(Engine.L, 1);
	}
	Check(
		!RemoteSecurityTarget->GetAttributeValue("Compromised"),
		"Remote-triggered deferred work cannot mutate the DataModel through ambient Core authority"
	);
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

	auto ServerFunction = std::make_shared<RemoteFunction>();
	Visible.insert(ServerFunction->GetObjectId());
	Check(
		ServerFunction->BindRemoteManager(&Server) &&
			Server.PublishRemote(Connection, ServerFunction->GetObjectId()),
		"server RemoteFunction is bound and visible"
	);
	PushGlobal(Engine.L, "ServerFunction", ServerFunction);
	{
		ScriptSecurityScope Scope(
			{ScriptExecutionDomain::Server,
			 {ScriptCapability::ReadDataModel, ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
		);
		Check(
			Run(Engine, R"(
				WaitHandlerContinued = false
				ServerFunction:SetServerHandler(function(Peer)
					task.wait(0)
					WaitHandlerContinued = true
					return 1
				end)
			)") == LUA_OK,
			"server RemoteFunction installs a bounded request handler"
		);
	}
	RemoteMessage WaitRequest{
		.Kind = RemoteMessageKind::Request,
		.Remote = ServerFunction->GetObjectId(),
		.Request = RemoteRequestId(1),
		.Deadline = 1s,
	};
	Check(
		Server.HandleTransportEvent(Incoming(Connection, WaitRequest)) && Server.Pump() == 1,
		"RemoteFunction request reaches its Luau handler"
	);
	Engine.Threads.Step();
	lua_getglobal(Engine.L, "WaitHandlerContinued");
	Check(
		!lua_toboolean(Engine.L, -1),
		"unsupported task.wait cannot leave a failed RemoteFunction handler scheduled to continue later"
	);
	lua_pop(Engine.L, 1);
	Check(!ServerScheduler.Messages.empty(), "failed RemoteFunction handler emits one bounded terminal response");
	if (!ServerScheduler.Messages.empty()) {
		auto HandlerFailure = DecodeRemoteMessage(ServerScheduler.Messages.back().Payload());
		Check(
			HandlerFailure && HandlerFailure->Kind == RemoteMessageKind::RequestError && HandlerFailure->Error &&
				HandlerFailure->Error->Code == "handler_error",
			"task.wait rejection becomes a sanitized RemoteFunction handler error"
		);
	}

	{
		auto LifetimeGame = std::make_shared<DataModel>();
		auto LifetimeEngine = std::make_unique<ScriptEngine>(LifetimeGame);
		auto LifetimeFunction = std::make_shared<RemoteFunction>();
		Visible.insert(LifetimeFunction->GetObjectId());
		Check(
			LifetimeFunction->BindRemoteManager(&Server) &&
				Server.PublishRemote(Connection, LifetimeFunction->GetObjectId()),
			"lifetime RemoteFunction is bound and visible"
		);
		PushGlobal(LifetimeEngine->L, "LifetimeFunction", LifetimeFunction);
		{
			ScriptSecurityScope Scope(
				{ScriptExecutionDomain::Server,
				 {ScriptCapability::ReadDataModel, ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
			);
			Check(
				Run(*LifetimeEngine, "LifetimeFunction:SetServerHandler(function(Peer) return 1 end)") == LUA_OK,
				"lifetime RemoteFunction installs a handler"
			);
		}
		LifetimeEngine.reset();
		LifetimeFunction.reset();
		Check(true, "RemoteFunction can outlive its ScriptEngine without accessing a closed Luau VM");
	}
	{
		auto ReentryGame = std::make_shared<DataModel>();
		auto ManagedRuntime = std::make_shared<ScriptEngine>(ReentryGame);
		auto ReentryFunction = std::make_shared<RemoteFunction>();
		ReentryFunction->SetParent(ReentryGame);
		Visible.insert(ReentryFunction->GetObjectId());
		Check(ReentryFunction->BindRemoteManager(&Server) &&
			Server.PublishRemote(Connection, ReentryFunction->GetObjectId()),
			"reentrant RemoteFunction is bound and visible");
		PushGlobal(ManagedRuntime->L, "ReentryFunction", ReentryFunction);
		{
			ScriptSecurityScope Scope(ScriptSecurityContext::ServerRuntime());
			Check(Run(*ManagedRuntime,
				"ReentryFunction:SetServerHandler(function(Peer) "
				"local Ok = pcall(function() game:Destroy() end); "
				"return Ok and 'unexpected_allow' or 'denied' end)") == LUA_OK,
				"restricted RemoteFunction installs a server handler");
		}
		std::weak_ptr<ScriptEngine> RuntimeWeak = ManagedRuntime;
		RemoteMessage ReentryRequest{
			.Kind = RemoteMessageKind::Request,
			.Remote = ReentryFunction->GetObjectId(),
			.Request = RemoteRequestId(9001),
			.Deadline = 1s,
		};
		Check(Server.HandleTransportEvent(Incoming(Connection, ReentryRequest)) && Server.Pump() == 1,
			"network pump resumes a restricted server handler");
		bool DeniedResponse = false;
		if (!ServerScheduler.Messages.empty()) {
			const auto Reply = DecodeRemoteMessage(ServerScheduler.Messages.back().Payload());
			DeniedResponse = Reply && Reply->Kind == RemoteMessageKind::Response && Reply->Arguments.size() == 1 &&
				std::holds_alternative<std::string>(Reply->Arguments.front()) &&
				std::get<std::string>(Reply->Arguments.front()) == "denied";
		}
		Check(DeniedResponse &&
			!ReentryGame->GetDestroyed() && ManagedRuntime && !RuntimeWeak.expired(),
			"network-only handler cannot destroy game and leaves its runtime alive");
		ManagedRuntime.reset();
		Check(RuntimeWeak.expired(), "server handler VM closes after ordinary owner release");
	}
	{
		RecordingScheduler OutboundScheduler;
		std::set<ObjectId> OutboundVisible;
		auto OutboundTime = std::chrono::steady_clock::time_point{};
		RemoteManager OutboundManager(
			RemoteManagerRole::Client, OutboundScheduler,
			[&](ConnectionId, ObjectId Object) { return OutboundVisible.contains(Object); },
			[](ObjectId Object) { return ObjectRegistry::Get().Lookup(Object); },
			[&] { return OutboundTime; }
		);
		Check(OutboundManager.AddPeer(Connection, ReplicationEpoch(17), Limits()),
			"outbound lifetime peer is admitted");
		auto OutboundGame = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(64.0f, 64.0f));
		gargantuan::Engine Runtime(OutboundGame, &Renderer, {}, EngineProviderConfiguration{.AudioEnabled = false});
		auto OutboundFunction = std::make_shared<RemoteFunction>();
		OutboundVisible.insert(OutboundFunction->GetObjectId());
		Check(OutboundFunction->BindRemoteManager(&OutboundManager, Connection) &&
			OutboundManager.PublishRemote(Connection, OutboundFunction->GetObjectId()),
			"outbound lifetime RemoteFunction is published");
		PushGlobal(Runtime.Script->L, "OutboundFunction", OutboundFunction);
		{
			ScriptSecurityScope Scope(
				{ScriptExecutionDomain::Client, {ScriptCapability::NetworkSend, ScriptCapability::NetworkReceive}}
			);
			Check(Run(*Runtime.Script, R"(
				local Thread = coroutine.create(function()
					OutboundFunction:InvokeServerWithTimeout(0.1)
				end)
				return coroutine.resume(Thread)
			)", 1) == LUA_OK && lua_toboolean(Runtime.Script->L, -1),
				"outbound request suspends a managed Luau coroutine");
			lua_settop(Runtime.Script->L, 0);
		}
		Check(OutboundManager.GetMetrics().InFlightRequests == 1,
			"outbound request remains pending before Engine destruction");
		std::weak_ptr<ScriptEngine> RuntimeWeak = Runtime.Script;
		Runtime.Destroy();
		Check(RuntimeWeak.expired(), "Engine destruction closes the pending request VM");
		OutboundTime += 1s;
		(void)OutboundManager.Pump();
		Check(OutboundManager.GetMetrics().InFlightRequests == 0,
			"timeout retires the outstanding request after its VM has closed");
	}

	{
		auto Sender = std::make_shared<Player>();
		const RemotePeerContext SenderPeer{Connection, ReplicationEpoch(9)};
		Server.SetPlayerResolvers(
			[&](const RemotePeerContext &) { return Sender->GetDestroyed() ? nullptr : Sender; },
			[&](const Player &Value) -> std::optional<RemotePeerContext> {
				return &Value == Sender.get() && !Value.GetDestroyed() ? std::optional{SenderPeer} : std::nullopt;
			}
		);
		PushGlobal(Engine.L, "Sender", Sender);
		Check(Server.AddPeer({8, 2}, ReplicationEpoch(10), Limits()),
			"Player submission fixture includes an unpublished peer");
		{
			ScriptSecurityScope Scope(ScriptSecurityContext::ServerRuntime());
			CheckLuau(Engine, R"(
				local Accepted, Code = ServerRemote:TryFireClient(Sender, "direct")
				assert(Accepted and Code == "accepted", "direct submission returned " .. tostring(Code))
				assert(not pcall(function() ServerRemote:TryFireClient(ServerRemote, "wrong class") end))
				local Count, Rejected, Reasons = ServerRemote:TryFireAllClients("partial")
				assert(Count == 1 and Rejected == 1 and Reasons.unpublished_remote == 1,
					string.format("broadcast accepted=%s rejected=%s unpublished=%s",
						tostring(Count), tostring(Rejected), tostring(Reasons.unpublished_remote)))
			)", "Luau event API exposes successful target and partial fanout feedback");
		}
		const auto DestroySender = ServerEvent->GetOnPlayerEvent()->Connect([&](std::any) { Sender->Destroy(); });
		{
			ScriptSecurityScope Scope(ScriptSecurityContext::ServerRuntime());
			CheckLuau(Engine, R"(
				PlayerHandlerRan = false
				ServerRemote.OnPlayerEvent:Once(function(Player) PlayerHandlerRan = true end)
			)", "Luau Player event accepts ordinary signal connections");
		}
		Check(Server.HandleTransportEvent(Incoming(Connection, EventMessage)) && Server.Pump() == 1,
			"Player event enters its normal dispatch safe point");
		lua_getglobal(Engine.L, "PlayerHandlerRan");
		Check(Sender->GetDestroyed() && !lua_toboolean(Engine.L, -1),
			"retiring Player in an earlier callback prevents later Player callbacks");
		lua_pop(Engine.L, 1);
		DestroySender->Disconnect();
		{
			ScriptSecurityScope Scope(ScriptSecurityContext::ServerRuntime());
			CheckLuau(Engine, R"(
				local Sent, Status = ServerRemote:TryFireClient(Sender)
				assert(not Sent and Status == "invalid_peer", "stale event target returned " .. tostring(Status))
				local Invoked, InvokeStatus, Message = ServerFunction:TryInvokeClient(Sender, 1)
				assert(not Invoked and InvokeStatus == "invalid_peer" and type(Message) == "string",
					"stale invocation target returned " .. tostring(InvokeStatus))
			)", "destroyed Player returns a stable invalid-peer outcome without submission");
		}
		Server.SetPlayerResolvers({}, {});
	}
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
