#pragma once

#include "ContentScaleFixture.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/RemoteFunction.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"

namespace gargantuan::test {
	// Observer only: every event is delivered unchanged to the real GameSession.
	class ObservedScaleTransport final : public network::IGameTransport {
		std::shared_ptr<network::IGameTransport> Delegate;
		ContentScalePeer &Observation;
	  public:
		ObservedScaleTransport(std::shared_ptr<network::IGameTransport> Input, ContentScalePeer &Output)
			: Delegate(std::move(Input)), Observation(Output) {}
		network::TransportOperationResult Start(const network::TransportStartConfiguration &Configuration) override { return Delegate->Start(Configuration); }
		network::TransportOperationResult Stop(network::DisconnectInfo Information) override { return Delegate->Stop(std::move(Information)); }
		network::TransportOperationResult Disconnect(network::ConnectionId Connection, network::DisconnectInfo Information) override { return Delegate->Disconnect(Connection, std::move(Information)); }
		network::TransportOperationResult Send(const network::NetworkMessageIntent &Message) override { return Delegate->Send(Message); }
		std::size_t PollEvents(std::span<network::TransportEvent> Output) override {
			const auto Count = Delegate->PollEvents(Output);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				if (const auto *Message = std::get_if<network::ReceivedMessageEvent>(&Output[Index])) Observation.Observe(Message->Payload, Message->Delivery);
				if (const auto *Disconnected = std::get_if<network::DisconnectedEvent>(&Output[Index]))
					Observation.DisconnectDiagnostic = Disconnected->Information.Diagnostic;
			}
			return Count;
		}
		std::optional<std::size_t> GetAvailableDatagramBytes(network::ConnectionId Connection) const override { return Delegate->GetAvailableDatagramBytes(Connection); }
		std::optional<network::NetworkStatistics> GetStatistics(network::ConnectionId Connection) const override { return Delegate->GetStatistics(Connection); }
	};

	inline void AddScaleGameplay(const std::shared_ptr<DataModel> &World, bool MeasureWithoutDiagnosticBroadcast = false) {
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		DiskFilesystem Filesystem(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT));
		Assets->LoadProjectAssets(Filesystem);
		auto Event = std::make_shared<RemoteEvent>();
		Event->SetName("ScaleEvent"); Event->SetParent(World);
		auto Function = std::make_shared<RemoteFunction>();
		Function->SetName("ScaleFunction"); Function->SetParent(World);
		auto ServerScript = std::make_shared<Script>();
		ServerScript->SetName("ScaleServerPolicy");
		ServerScript->SetRunContext(Enums::RunContext::Server);
		ServerScript->SetSource(std::string("local ReplicateDiagnosticCounter = ") +
			(MeasureWithoutDiagnosticBroadcast ? "false\n" : "true\n") + R"(
local Control = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local Function = game:FindFirstChild("ScaleFunction")
local Event = game:FindFirstChild("ScaleEvent")
assert(Control:RegisterAction("ScaleLunge", "asset://d9d9e9649adbad59588d137c2a642e1d", 0.5, Vector3.new(0.9, 0, 0), 0, true))
Control:SetActionPolicy(function(Player, Character, Name)
    return Player.Character == Character and Name == "ScaleLunge"
end)
Event.OnServerEvent:Connect(function(Peer, Message, Sequence)
    if ReplicateDiagnosticCounter then
        Control:SetAttribute("ScaleEvents", (Control:GetAttribute("ScaleEvents") or 0) + 1)
    end
    Event:FireClient(Peer.Slot, Peer.Generation, Message, Sequence)
end)
Function:SetServerHandler(function(Peer, Message) return Message end)
)");
		ServerScript->SetParent(World);
		auto ClientScript = std::make_shared<Script>();
		ClientScript->SetName("ScaleClientTraffic");
		ClientScript->SetRunContext(Enums::RunContext::Client);
		ClientScript->SetSource(R"(
local Control = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Function = game:FindFirstChild("ScaleFunction")
local Event = game:FindFirstChild("ScaleEvent")
assert(Control:RegisterAction("ScaleLunge", "asset://d9d9e9649adbad59588d137c2a642e1d", 0.5, Vector3.new(0.9, 0, 0), 0, true))
local Phase = nil
Control:SetAttribute("ScaleClientStarted", true)
local Tick = 0
local StartedActionAt = 0
local Resolutions = 0
local Endings = 0
local EventSequence = 0
local EventPending = {}
local EventSamples = {}
local ActionSamples = {}
local LastEventAck = 0
local EventMaxGapUs = 0
Event.OnClientEvent:Connect(function(Message, Sequence)
    local Started = EventPending[Sequence]
    if not Started then return end
    EventPending[Sequence] = nil
    if Message ~= Phase then return end
    local Now = os.clock()
    if LastEventAck ~= 0 then EventMaxGapUs = math.max(EventMaxGapUs, (Now - LastEventAck) * 1000000) end
    LastEventAck = Now
    if #EventSamples < 1200 then table.insert(EventSamples, (Now - Started) * 1000000) end
    Control:SetAttribute("ScaleEventAcks", (Control:GetAttribute("ScaleEventAcks") or 0) + 1)
end)
local function Metrics(Kind, Samples)
    if #Samples == 0 then return "[Content:Scale" .. Kind .. "] samples=0" end
    table.sort(Samples)
    local Total = 0
    for _, Value in Samples do Total += Value end
    local function P(Fraction) return Samples[math.floor((#Samples - 1) * Fraction) + 1] end
    return string.format("[Content:Scale%s] phase=%s samples=%d mean_us=%.0f p50_us=%.0f p95_us=%.0f p99_us=%.0f max_us=%.0f",
        Kind, Phase, #Samples, Total / #Samples, P(0.50), P(0.95), P(0.99), Samples[#Samples])
end
Control.ActionResolved:Connect(function(_, Name, Accepted)
    if Name ~= "ScaleLunge" or not Accepted then
        Control:SetAttribute("ScaleActionRejections", (Control:GetAttribute("ScaleActionRejections") or 0) + 1)
        return
    end
    Resolutions += 1
    if #ActionSamples < 64 then table.insert(ActionSamples, (os.clock() - StartedActionAt) * 1000000) end
    Control:SetAttribute("ScaleActionResolutions", Resolutions)
    Control:SetAttribute("ScaleActionMaxResultUs", math.max(Control:GetAttribute("ScaleActionMaxResultUs") or 0, (os.clock() - StartedActionAt) * 1000000))
end)
Control.ActionEnded:Connect(function(_, Name)
    if Name ~= "ScaleLunge" then
        Control:SetAttribute("ScaleActionUnexpectedEndings", (Control:GetAttribute("ScaleActionUnexpectedEndings") or 0) + 1)
        return
    end
    Endings += 1
    Control:SetAttribute("ScaleActionEndings", Endings)
end)
RunService.PostSimulation:Connect(function()
    if not Players.LocalPlayer or not Players.LocalPlayer.Character then return end
    local NextPhase = Control:GetAttribute("ScalePhase")
    if not NextPhase then return end
    Tick += 1
    if Tick % 40 == 1 then
        StartedActionAt = os.clock()
        if not Control:RequestAction("ScaleLunge") then
            Control:SetAttribute("ScaleActionSubmissionFailures", (Control:GetAttribute("ScaleActionSubmissionFailures") or 0) + 1)
        end
    end
    if EventSequence < Tick + 1 then
        EventSequence += 1
        -- Both the offered count and pending map are bounded by the phase's
        -- 1,200-tick cap; acknowledged entries are retired immediately.
        EventPending[EventSequence] = os.clock()
        Event:FireServer(NextPhase, EventSequence)
    end
    if NextPhase == Phase then return end
    Phase = NextPhase
    EventSamples = {}
    ActionSamples = {}
    EventPending = {}
    LastEventAck = 0
    EventMaxGapUs = 0
    local RunningPhase = Phase
    task.spawn(function()
        local Samples = {}
        local Total = 0
        local Errors = 0
        local Timeouts = 0
        for Index = 1, 100 do
            local Started = os.clock()
            local Ok, Value, Status = pcall(function() return Function:InvokeServerWithTimeout(5, RunningPhase) end)
            if not Ok or Value ~= RunningPhase then Errors += 1 end
            if (Ok and Value == nil and Status == "timeout") or
                (not Ok and type(Value) == "string" and string.find(Value, "timeout", 1, true)) then Timeouts += 1 end
            local Elapsed = (os.clock() - Started) * 1000000
            Total += Elapsed
            table.insert(Samples, Elapsed)
            Control:SetAttribute("ScaleRemoteSamples", Index)
            Control:SetAttribute("ScaleRemoteErrors", Errors)
            task.wait()
        end
        table.sort(Samples)
        Control:SetAttribute("ScaleRemoteMetrics", string.format("[Content:ScaleRemote] phase=%s samples=100 mean_us=%.0f p50_us=%.0f p95_us=%.0f p99_us=%.0f max_us=%.0f timeouts=%d errors=%d", RunningPhase, Total / 100, Samples[50], Samples[95], Samples[99], Samples[100], Timeouts, Errors))
        Control:SetAttribute("ScaleRemoteErrors", Errors)
        Control:SetAttribute("ScaleRemoteP95Us", Samples[95])
        Control:SetAttribute("ScaleRemoteP99Us", Samples[99])
        Control:SetAttribute("ScaleRemoteMaxUs", Samples[100])
        Control:SetAttribute("ScaleEventMetrics", Metrics("Event", EventSamples))
        Control:SetAttribute("ScaleActionMetrics", Metrics("Action", ActionSamples))
        Control:SetAttribute("ScaleEventMaxGapUs", EventMaxGapUs)
        Control:SetAttribute("ScaleRemoteDone", RunningPhase)
    end)
end)
)");
		ClientScript->SetParent(World);
	}
}
