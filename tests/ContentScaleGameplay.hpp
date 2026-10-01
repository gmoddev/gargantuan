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

	inline void AddScaleGameplay(const std::shared_ptr<DataModel> &World, bool MeasureWithoutDiagnosticBroadcast = false,
		bool Qualified = false, std::uint32_t ProducerPlayerId = 0, bool PhysicalFarm = false) {
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		DiskFilesystem Filesystem(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT));
		Assets->LoadProjectAssets(Filesystem);
		auto Event = std::make_shared<RemoteEvent>();
		Event->SetName("ScaleEvent"); Event->SetParent(World);
		auto Function = std::make_shared<RemoteFunction>();
		Function->SetName("ScaleFunction"); Function->SetParent(World);
		if (PhysicalFarm) {
			auto PhaseControl = std::make_shared<RemoteEvent>();
			PhaseControl->SetName("ScalePhaseControl"); PhaseControl->SetParent(World);
		}
		auto ServerScript = std::make_shared<Script>();
		ServerScript->SetName("ScaleServerPolicy");
		ServerScript->SetRunContext(Enums::RunContext::Server);
		ServerScript->SetSource(std::string("local ReplicateDiagnosticCounter = ") +
			(MeasureWithoutDiagnosticBroadcast ? "false\n" : "true\n") +
			"local PhysicalFarm = " + (PhysicalFarm ? "true\n" : "false\n") + R"(
local Control = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local Function = game:FindFirstChild("ScaleFunction")
local Event = game:FindFirstChild("ScaleEvent")
if PhysicalFarm then
    local PhaseControl = game:FindFirstChild("ScalePhaseControl")
    local RunService = game:GetService("RunService")
    local CurrentPhase = nil
    local PendingCompletion = nil
    local PhaseAcknowledgements = {}
    local AcknowledgementCount = 0
    local ContentObservations = {}
    local ContentObservationCount = 0
    local PhaseTick = 0
    local LastBroadcastTick = -60
    local LastStopTick = -60
    PhaseControl.OnServerEvent:Connect(function(Peer, Kind, Phase)
        if Kind == "ready" then
            if CurrentPhase then PhaseControl:FireClient(Peer.Slot, Peer.Generation, "phase", CurrentPhase) end
        elseif Kind == "phase_ack" and Phase == CurrentPhase then
            local PeerKey = tostring(Peer.Slot) .. ":" .. tostring(Peer.Generation)
            if not PhaseAcknowledgements[PeerKey] then
                PhaseAcknowledgements[PeerKey] = true
                AcknowledgementCount += 1
            end
        elseif Kind == "content_observed" and Phase == CurrentPhase then
            local PeerKey = tostring(Peer.Slot) .. ":" .. tostring(Peer.Generation)
            if not ContentObservations[PeerKey] then
                ContentObservations[PeerKey] = true
                ContentObservationCount += 1
                print(string.format("[Content:ScalePhase] event=content_observed phase=%s peer_slot=%d peer_generation=%d count=%d",
                    Phase, Peer.Slot, Peer.Generation, ContentObservationCount))
            end
        elseif Kind == "complete" and Phase == CurrentPhase and
            game:GetAttribute("ScalePhaseStopRequested") == CurrentPhase and
            Peer.Slot == game:GetAttribute("ScaleProducerConnectionSlot") and
            Peer.Generation == game:GetAttribute("ScaleProducerConnectionGeneration") then
            PendingCompletion = Phase
            print(string.format("[Content:ScalePhase] event=producer_complete phase=%s peer_slot=%d peer_generation=%d",
                Phase, Peer.Slot, Peer.Generation))
        elseif Kind == "failed" and Phase == CurrentPhase and
            Peer.Slot == game:GetAttribute("ScaleProducerConnectionSlot") and
            Peer.Generation == game:GetAttribute("ScaleProducerConnectionGeneration") then
            Control:SetAttribute("ScaleProducerFailed", Phase)
        end
    end)
    RunService.PostSimulation:Connect(function()
        PhaseTick += 1
        local NextPhase = game:GetAttribute("ScalePhase")
        if type(NextPhase) == "string" and NextPhase ~= CurrentPhase then
            CurrentPhase = NextPhase
            PendingCompletion = nil
            PhaseAcknowledgements = {}
            AcknowledgementCount = 0
            ContentObservations = {}
            ContentObservationCount = 0
            Control:SetAttribute("ScalePhaseAcks", 0)
            Control:SetAttribute("ScaleContentObservedAcks", 0)
            Control:SetAttribute("ScaleRemoteDone", nil)
            Control:SetAttribute("ScaleProducerFailed", nil)
            LastBroadcastTick = PhaseTick - 60
            LastStopTick = PhaseTick - 60
        end
        if CurrentPhase and AcknowledgementCount < 32 and PhaseTick - LastBroadcastTick >= 60 then
            LastBroadcastTick = PhaseTick
            local Ok = pcall(function() PhaseControl:FireAllClients("phase", CurrentPhase) end)
            if Ok then print(string.format("[Content:ScalePhase] event=broadcast phase=%s", CurrentPhase)) end
        end
        if CurrentPhase and game:GetAttribute("ScalePhaseStopRequested") == CurrentPhase and
            PendingCompletion ~= CurrentPhase and PhaseTick - LastStopTick >= 60 then
            LastStopTick = PhaseTick
            local Slot = game:GetAttribute("ScaleProducerConnectionSlot")
            local Generation = game:GetAttribute("ScaleProducerConnectionGeneration")
            if type(Slot) == "number" and type(Generation) == "number" then
                local Ok = pcall(function() PhaseControl:FireClient(Slot, Generation, "stop", CurrentPhase) end)
                if Ok then print(string.format("[Content:ScalePhase] event=stop_sent phase=%s", CurrentPhase)) end
            end
        end
        if PendingCompletion == CurrentPhase and CurrentPhase then
            Control:SetAttribute("ScaleRemoteDone", CurrentPhase)
            PendingCompletion = nil
        end
        if CurrentPhase then
            Control:SetAttribute("ScalePhaseAcks", AcknowledgementCount)
            Control:SetAttribute("ScaleContentObservedAcks", ContentObservationCount)
        end
    end)
end
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
		ClientScript->SetSource(std::string("local Qualified = ") + (Qualified ? "true\n" : "false\n") +
			"local ProducerPlayerId = " + std::to_string(ProducerPlayerId) + "\n" +
			"local PhysicalFarm = " + (PhysicalFarm ? "true\n" : "false\n") + R"(
local Control = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Workspace = if PhysicalFarm then game:GetService("Workspace") else nil
local Function = game:FindFirstChild("ScaleFunction")
local Event = game:FindFirstChild("ScaleEvent")
local PhaseControl = if PhysicalFarm then game:FindFirstChild("ScalePhaseControl") else nil
assert(Control:RegisterAction("ScaleLunge", "asset://d9d9e9649adbad59588d137c2a642e1d", 0.5, Vector3.new(0.9, 0, 0), 0, true))
local Phase = nil
local PendingPhaseAcknowledgement = nil
local PendingPhaseCompletion = nil
local PendingPhaseCompletionHealthy = true
local PendingPhaseStop = nil
local PhaseReadySent = false
local ContentPhases = {"baseline", "load", "resident", "evict", "reload"}
local ContentObservedPhases = 0
local ContentObservedPhase = nil
local ContentObservationAttempts = 0
local ContentObservationSubmitted = false
local LastContentObservationTick = -60
local LastContentCheckTick = -6
local PhaseControlTick = 0
if PhaseControl then
    PhaseControl.OnClientEvent:Connect(function(Kind, Phase)
        if Kind == "phase" and type(Phase) == "string" then
            Control:SetAttribute("ScalePhase", Phase)
            PendingPhaseAcknowledgement = Phase
            print(string.format("[Content:ScalePhase] event=received phase=%s", Phase))
        elseif Kind == "stop" and Phase == Control:GetAttribute("ScalePhase") then
            PendingPhaseStop = Phase
            print(string.format("[Content:ScalePhase] event=stop_received phase=%s", Phase))
        end
    end)
end
Control:SetAttribute("ScaleClientStarted", true)
local Tick = 0
local StartedActionAt = 0
local Resolutions = 0
local Endings = 0
local EventSequence = 0
local EventPending = nil
local EventOffers = 0
local EventAcks = 0
local EventSamples = {}
local ActionSamples = {}
local LastEventAck = 0
local EventMaxGapUs = 0
local EventMaxRttUs = 0
local ActionRequests = 0
local ActionPendingResult = false
local ActionPendingEndings = 0
local ActionSubmissionFailures = 0
local ActionRejections = 0
local ActionUnexpectedEndings = 0
local ActionMaxResultUs = 0
local PhaseEventOffersStart = 0
local PhaseEventAcksStart = 0
local PhaseActionRequestsStart = 0
local PhaseActionResolutionsStart = 0
local PhaseActionEndingsStart = 0
local PhaseActionSubmissionFailuresStart = 0
local PhaseActionRejectionsStart = 0
local PhaseActionUnexpectedEndingsStart = 0
local PhaseStartedAt = 0
local PhaseStopping = false
local StopDrainStartedAt = 0
local PhaseFinalized = false
local PhaseTrafficFailure = false
local RpcComplete = false
local RpcSamples = {}
local RpcErrors = 0
local RpcTimeouts = 0
local ProducerReported = false
Event.OnClientEvent:Connect(function(Message, Sequence)
    if not EventPending or EventPending.Sequence ~= Sequence or EventPending.Phase ~= Message then
        if PhysicalFarm then PhaseTrafficFailure = true end
        return
    end
    local Started = EventPending.Started
    EventPending = nil
    local Now = os.clock()
    if LastEventAck ~= 0 then EventMaxGapUs = math.max(EventMaxGapUs, (Now - LastEventAck) * 1000000) end
    LastEventAck = Now
    local RttUs = (Now - Started) * 1000000
    EventMaxRttUs = math.max(EventMaxRttUs, RttUs)
    if #EventSamples < 1200 then table.insert(EventSamples, RttUs) end
    EventAcks += 1
    Control:SetAttribute("ScaleEventAcks", EventAcks)
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
        ActionRejections += 1
        Control:SetAttribute("ScaleActionRejections", ActionRejections)
        if PhysicalFarm then
            PhaseTrafficFailure = true
            ActionPendingResult = false
            if ActionPendingEndings > 0 then ActionPendingEndings -= 1 end
        end
        return
    end
    if PhysicalFarm and not ActionPendingResult then PhaseTrafficFailure = true end
    ActionPendingResult = false
    Resolutions += 1
    local ResultUs = (os.clock() - StartedActionAt) * 1000000
    if #ActionSamples < 64 then table.insert(ActionSamples, ResultUs) end
    Control:SetAttribute("ScaleActionResolutions", Resolutions)
    ActionMaxResultUs = math.max(ActionMaxResultUs, ResultUs)
    Control:SetAttribute("ScaleActionMaxResultUs", ActionMaxResultUs)
end)
Control.ActionEnded:Connect(function(_, Name)
    if Name ~= "ScaleLunge" then
        ActionUnexpectedEndings += 1
        Control:SetAttribute("ScaleActionUnexpectedEndings", ActionUnexpectedEndings)
        return
    end
    if PhysicalFarm then
        if ActionPendingEndings == 0 then
            ActionUnexpectedEndings += 1
            Control:SetAttribute("ScaleActionUnexpectedEndings", ActionUnexpectedEndings)
            PhaseTrafficFailure = true
            return
        end
        ActionPendingEndings -= 1
    end
    Endings += 1
    Control:SetAttribute("ScaleActionEndings", Endings)
end)
local function PublishPhaseMetrics(RunningPhase)
    table.sort(RpcSamples)
    local Total = 0
    for _, Value in RpcSamples do Total += Value end
    local Mean = if #RpcSamples == 0 then 0 else Total / #RpcSamples
    local P95 = RpcSamples[95] or 0
    local P99 = RpcSamples[99] or 0
    local Maximum = RpcSamples[100] or 0
    Control:SetAttribute("ScaleRemoteMetrics", string.format("[Content:ScaleRemote] phase=%s samples=%d mean_us=%.0f p50_us=%.0f p95_us=%.0f p99_us=%.0f max_us=%.0f timeouts=%d errors=%d",
        RunningPhase, #RpcSamples, Mean, RpcSamples[50] or 0, P95, P99, Maximum, RpcTimeouts, RpcErrors))
    Control:SetAttribute("ScaleRemoteSamples", #RpcSamples)
    Control:SetAttribute("ScaleRemoteP95Us", P95)
    Control:SetAttribute("ScaleRemoteP99Us", P99)
    Control:SetAttribute("ScaleRemoteMaxUs", Maximum)
    Control:SetAttribute("ScaleRemoteErrors", RpcErrors)
    Control:SetAttribute("ScaleRemoteTimeouts", RpcTimeouts)
    Control:SetAttribute("ScaleEventMetrics", Metrics("Event", EventSamples))
    Control:SetAttribute("ScaleEventOffers", EventOffers)
    Control:SetAttribute("ScaleEventAcks", EventAcks)
    Control:SetAttribute("ScaleEventMaxRttUs", EventMaxRttUs)
    Control:SetAttribute("ScaleEventMaxGapUs", EventMaxGapUs)
    Control:SetAttribute("ScaleEventOutstanding", if EventPending then 1 else 0)
    Control:SetAttribute("ScaleActionMetrics", Metrics("Action", ActionSamples))
    Control:SetAttribute("ScaleActionRequests", ActionRequests)
    Control:SetAttribute("ScaleActionResolutions", Resolutions)
    Control:SetAttribute("ScaleActionEndings", Endings)
    Control:SetAttribute("ScaleActionMaxResultUs", ActionMaxResultUs)
    Control:SetAttribute("ScaleActionSubmissionFailures", ActionSubmissionFailures)
    Control:SetAttribute("ScaleActionRejections", ActionRejections)
    Control:SetAttribute("ScaleActionUnexpectedEndings", ActionUnexpectedEndings)
    local Healthy = #RpcSamples == 100 and RpcErrors == 0 and RpcTimeouts == 0 and
        P95 <= 150000 and P99 <= 250000 and Maximum <= 500000 and not PhaseTrafficFailure and
        EventOffers > PhaseEventOffersStart and
        EventOffers - PhaseEventOffersStart == EventAcks - PhaseEventAcksStart and
        not EventPending and EventMaxRttUs <= 250000 and EventMaxGapUs <= 250000 and
        ActionRequests > PhaseActionRequestsStart and
        ActionRequests - PhaseActionRequestsStart == Resolutions - PhaseActionResolutionsStart and
        ActionRequests - PhaseActionRequestsStart == Endings - PhaseActionEndingsStart and
        not ActionPendingResult and ActionPendingEndings == 0 and ActionMaxResultUs <= 250000 and
        ActionSubmissionFailures == PhaseActionSubmissionFailuresStart and
        ActionRejections == PhaseActionRejectionsStart and
        ActionUnexpectedEndings == PhaseActionUnexpectedEndingsStart
    Control:SetAttribute("ScaleProducerHealthy", Healthy)
    Control:SetAttribute("ScaleMetricsPhase", RunningPhase)
    -- This marker is last: the host cannot observe a complete phase with a
    -- partially published typed metrics snapshot.
    Control:SetAttribute("ScaleRemoteDone", RunningPhase)
    return Healthy
end
RunService.PostSimulation:Connect(function()
    local LocalPlayer = Players.LocalPlayer
    if not LocalPlayer then return end
    if PhaseControl then
        PhaseControlTick += 1
        if not PhaseReadySent then
            PhaseReadySent = pcall(function() PhaseControl:FireServer("ready", "") end)
        end
        if PendingPhaseAcknowledgement then
            local Acknowledged = pcall(function()
                PhaseControl:FireServer("phase_ack", PendingPhaseAcknowledgement)
            end)
            if Acknowledged then PendingPhaseAcknowledgement = nil end
        end
        local ReceivedPhase = Control:GetAttribute("ScalePhase")
        if ReceivedPhase == ContentPhases[ContentObservedPhases + 1] and
            PhaseControlTick - LastContentCheckTick >= 6 then
            LastContentCheckTick = PhaseControlTick
            local Root = Workspace:FindFirstChild("ContentScaleRegion")
            local Objects = if Root then #Root:GetDescendants() + 1 else 0
            local ExpectsResident = ReceivedPhase == "load" or ReceivedPhase == "resident" or
                ReceivedPhase == "reload"
            -- Evict has already required zero objects before reload. The host
            -- independently verifies resident/reload ObjectId continuity.
            if (ExpectsResident and Objects == 512) or
                (not ExpectsResident and Objects == 0) then
                ContentObservedPhases += 1
                ContentObservedPhase = ReceivedPhase
                ContentObservationAttempts = 0
                ContentObservationSubmitted = false
                LastContentObservationTick = PhaseControlTick - 60
                print(string.format("[Content:ScalePhase] event=content_materialized phase=%s objects=%d player_id=%d",
                    ReceivedPhase, Objects, LocalPlayer.PlayerId))
            end
        end
        if ContentObservedPhase == ReceivedPhase and not ContentObservationSubmitted and
            ContentObservationAttempts < 20 and
            PhaseControlTick - LastContentObservationTick >= 60 then
            LastContentObservationTick = PhaseControlTick
            ContentObservationAttempts += 1
            local Observed = pcall(function()
                PhaseControl:FireServer("content_observed", ContentObservedPhase)
            end)
            if Observed then
                print(string.format("[Content:ScalePhase] event=content_ack_sent phase=%s player_id=%d attempt=%d",
                    ContentObservedPhase, LocalPlayer.PlayerId, ContentObservationAttempts))
                -- One reliable submission is the workload obligation. Retry only
                -- local submission failure; repeated successful ACKs add load.
                ContentObservationSubmitted = true
            end
        end
        if PendingPhaseCompletion then
            local Completed = pcall(function()
                PhaseControl:FireServer(if PendingPhaseCompletionHealthy then "complete" else "failed",
                    PendingPhaseCompletion)
            end)
            if Completed then PendingPhaseCompletion = nil end
        end
    end
    if not ProducerReported then
        ProducerReported = true
        local IsProducer = ProducerPlayerId == 0 or LocalPlayer.PlayerId == ProducerPlayerId
        Control:SetAttribute("ScaleTrafficProducer", IsProducer)
        Control:SetAttribute("ScaleTrafficPlayerId", LocalPlayer.PlayerId)
        print(string.format("[Content:ScaleProducer] player_id=%d selected=%s policy_player_id=%d",
            LocalPlayer.PlayerId, tostring(IsProducer), ProducerPlayerId))
    end
    local NextPhase = Control:GetAttribute("ScalePhase")
    if PhysicalFarm and NextPhase == "complete" then return end
    if ProducerPlayerId ~= 0 and LocalPlayer.PlayerId ~= ProducerPlayerId then return end
    if not LocalPlayer.Character then return end
    if not NextPhase then return end
    if NextPhase ~= Phase then
        if PhysicalFarm and Phase and not PhaseFinalized then
            PhaseTrafficFailure = true
            return
        end
        Phase = NextPhase
        PhaseStartedAt = os.clock()
        PhaseStopping = false
        StopDrainStartedAt = 0
        PhaseFinalized = false
        PhaseTrafficFailure = false
        PendingPhaseStop = nil
        EventSamples = {}
        ActionSamples = {}
        EventPending = nil
        LastEventAck = 0
        EventMaxGapUs = 0
        EventMaxRttUs = 0
        ActionMaxResultUs = 0
        ActionPendingResult = false
        ActionPendingEndings = 0
        PhaseEventOffersStart = EventOffers
        PhaseEventAcksStart = EventAcks
        PhaseActionRequestsStart = ActionRequests
        PhaseActionResolutionsStart = Resolutions
        PhaseActionEndingsStart = Endings
        PhaseActionSubmissionFailuresStart = ActionSubmissionFailures
        PhaseActionRejectionsStart = ActionRejections
        PhaseActionUnexpectedEndingsStart = ActionUnexpectedEndings
        RpcComplete = false
        RpcSamples = {}
        RpcErrors = 0
        RpcTimeouts = 0
        local RunningPhase = Phase
        task.spawn(function()
            for Index = 1, 100 do
                local Started = os.clock()
                local Ok, Value, Status = pcall(function() return Function:InvokeServerWithTimeout(5, RunningPhase) end)
                if not Ok or Value ~= RunningPhase then RpcErrors += 1 end
                if (Ok and Value == nil and Status == "timeout") or
                    (not Ok and type(Value) == "string" and string.find(Value, "timeout", 1, true)) then RpcTimeouts += 1 end
                table.insert(RpcSamples, (os.clock() - Started) * 1000000)
                Control:SetAttribute("ScaleRemoteSamples", Index)
                Control:SetAttribute("ScaleRemoteErrors", RpcErrors)
                task.wait(if Qualified then 0.1 else 0)
            end
            RpcComplete = true
            if not PhysicalFarm then
                PublishPhaseMetrics(RunningPhase)
                PhaseFinalized = true
            end
        end)
    end
    if PhaseFinalized then return end
    if PhysicalFarm and PendingPhaseStop == Phase and os.clock() - PhaseStartedAt > 13 then
        if not PhaseStopping then
            PhaseStopping = true
            StopDrainStartedAt = os.clock()
            print(string.format("[Content:ScalePhase] event=drain_start phase=%s offers=%d acks=%d action_requests=%d action_resolutions=%d action_endings=%d",
                Phase, EventOffers - PhaseEventOffersStart, EventAcks - PhaseEventAcksStart,
                ActionRequests - PhaseActionRequestsStart, Resolutions - PhaseActionResolutionsStart,
                Endings - PhaseActionEndingsStart))
        end
    end
    if PhaseStopping then
        local Drained = not EventPending and not ActionPendingResult and ActionPendingEndings == 0
        if not Drained and os.clock() - StopDrainStartedAt > 2 then PhaseTrafficFailure = true end
        if RpcComplete and (Drained or PhaseTrafficFailure) then
            PendingPhaseCompletionHealthy = PublishPhaseMetrics(Phase)
            PhaseFinalized = true
            PendingPhaseCompletion = Phase
            print(string.format("[Content:ScalePhase] event=metrics_final phase=%s healthy=%s elapsed_us=%.0f event_offers=%d event_acks=%d action_requests=%d action_resolutions=%d action_endings=%d",
                Phase, tostring(PendingPhaseCompletionHealthy), (os.clock() - PhaseStartedAt) * 1000000,
                EventOffers - PhaseEventOffersStart, EventAcks - PhaseEventAcksStart,
                ActionRequests - PhaseActionRequestsStart, Resolutions - PhaseActionResolutionsStart,
                Endings - PhaseActionEndingsStart))
        end
        return
    end
    Tick += 1
    if Tick % (if Qualified then 120 else 40) == 1 then
        if PhysicalFarm and (ActionPendingResult or ActionPendingEndings > 0) then
            PhaseTrafficFailure = true
        else
            StartedActionAt = os.clock()
            ActionPendingResult = true
            ActionRequests += 1
            if Control:RequestAction("ScaleLunge") then
                if PhysicalFarm then ActionPendingEndings += 1 end
            else
                ActionPendingResult = false
                ActionSubmissionFailures += 1
                Control:SetAttribute("ScaleActionSubmissionFailures", ActionSubmissionFailures)
            end
        end
    end
    if (not Qualified or Tick % 4 == 1) and not EventPending then
        EventSequence += 1
        EventOffers += 1
        EventPending = {Sequence = EventSequence, Phase = Phase, Started = os.clock()}
        Event:FireServer(Phase, EventSequence)
    end
end)
)");
		ClientScript->SetParent(World);
	}
}
