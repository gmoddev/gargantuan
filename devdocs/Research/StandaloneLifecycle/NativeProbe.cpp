// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
// Private research executable. No public Engine bindings are installed.
#include "gargantuan/Engine.hpp"
#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/gui/GuiRuntime.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/Players.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace gargantuan;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
    Clock::time_point AppTime{1s};
    Clock::time_point SimTime{1s};
    bool DispatchingApplication = false;

    void Check(bool Condition, const char *Reason) {
        if (!Condition) throw std::runtime_error(Reason);
    }

    template<class RuntimeType>
    void ConfigureClock(RuntimeType &Runtime, bool Application) {
        if constexpr (requires { Runtime.SetDevelopmentClock([] { return AppTime; }); })
            Runtime.SetDevelopmentClock([Application] { return Application ? AppTime : SimTime; });
    }

    std::shared_ptr<Script> Run(ScriptEngine &VM, const char *Code) {
        auto Entry = std::make_shared<Script>();
        Entry->SetRunContext(Enums::RunContext::Client);
        Entry->SetSource(Code);
        Entry->SetParent(VM.DataModel);
        VM.RunBootstrapScript(Entry);
        Check(Entry->Status != ScriptStatus::Error, Entry->ErrorMessage.c_str());
        return Entry;
    }

    void Click(GuiRuntime &Gui) {
        (void)Gui.Reconcile();
        for (const auto Action : {GuiPointerAction::Down, GuiPointerAction::Up}) {
            (void)Gui.ProcessPointer({1, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
                Action, Vector2(30, 20)});
        }
    }

    double Number(DataModel &Game, const char *Name) {
        const auto Label = std::dynamic_pointer_cast<TextButton>(Game.FindFirstChild(Name, true));
        Check(Label != nullptr, Name);
        return std::stod(Label->GetText());
    }
}

// The retained pre-debugger build has no clock injection API. Its scratch copy of
// just the two observation sites calls this function. Host policy deadlines stay real.
std::chrono::steady_clock::time_point ResearchClockNow() {
    return DispatchingApplication ? AppTime : SimTime;
}

int main() {
    try {
        BootstrapNativeRuntimeSchema();
        auto Presentation = std::make_shared<DataModel>();
        auto App = std::make_shared<ScriptEngine>(Presentation);
        ConfigureClock(App->Threads, true);
        GuiRuntime Gui(Presentation, std::filesystem::path(DefaultGuiFontPath));
        Gui.SetViewport({320, 180, 1, {}});
        DispatchingApplication = true;
        const auto Entry = Run(*App, R"(
            local Root = Instance.new("ScreenGui")
            Root.Name = "Title"
            Root.Parent = game
            local Button = Instance.new("TextButton")
            Button.Text = "0"
            Button.Size = UDim2.fromOffset(120, 50)
            Button.Parent = Root
            local Count = 0
            Button.Activated:Connect(function()
                Count += 1
                Button.Text = tostring(Count)
            end)
            task.delay(3, function() Button.Name = "ApplicationTimer" end)
        )");
        Click(Gui);
        auto Button = std::dynamic_pointer_cast<TextButton>(Presentation->FindFirstChildOfClass("TextButton", true));
        auto Menu = std::dynamic_pointer_cast<ScreenGui>(Presentation->FindFirstChild("Title", false));
        Check(Menu != nullptr, "registered screen");
        Check(Button && Button->GetText() == "1", "menu before session");
        Check(!Presentation->FindFirstChildOfClass("Player", true), "menu acquired Player");
        std::cout << "[Lifecycle:Probe] pregame_click=1 players=0\n";
        std::shared_ptr<Player> Previous;
        for (int Cycle = 1; Cycle <= 2; ++Cycle) {
            DispatchingApplication = false;
            auto World = std::make_shared<DataModel>();
            HeadlessRenderer Surface(Vector2(320, 180));
            Engine Session(World, &Surface, [](std::string Level, std::string Message) {
                if (Level == "Error") std::cerr << "[Lifecycle:Diagnostic] " << Message << '\n';
            }, EngineProviderConfiguration{.AudioEnabled=false});
            ConfigureClock(Session, false);
            Session.ProcessService->Alive = true;
            auto Owner = Session.Players->GetLocalPlayer();
            Check(Owner && *Owner != Previous && (*Owner)->GetPlayerGui(), "fresh offline owner/GUI");
            Previous = *Owner;
            Session.Workspace->SetGravity(9.8f);
            auto Falling = std::make_shared<Part>();
            Falling->SetCFrame(CFrame(0, 50, 0));
            Falling->SetAnchored(false);
            Falling->SetParent(Session.Workspace);
            Menu->SetEnabled(false);
            Click(Gui);
            Check(Button->GetText() == std::to_string(Cycle), "hidden screen accepted a click");
            Session.Step();
            Check(Session.GetSimulationTick() == 1, "offline host entered gameplay");
            const auto Timer = Run(*Session.Script, R"(
                local function Witness(Name)
                    local Button = Instance.new("TextButton")
                    Button.Name = Name
                    Button.Text = "0"
                    Button.Parent = script
                    return Button
                end
                local Frame, Ran, Wait = Witness("FrameMs"), Witness("Ran"), Witness("WaitMs")
                game:GetService("RunService").PreSimulation:Connect(function(Delta)
                    Frame.Text = tostring(Delta * 1000)
                end)
                task.delay(0.125, function()
                    Ran.Text = tostring(tonumber(Ran.Text) + 1)
                end)
                Wait.Text = tostring(task.wait(0.125) * 1000)
            )");
            SimTime += 32ms;
            Session.Step();
            const auto Tick = Session.GetSimulationTick();
            const auto HeldHeight = Falling->GetPosition().y;
            DispatchingApplication = true;
            AppTime += 60s;
            App->Step();
            Menu->SetEnabled(true);
            Click(Gui);
            Check(Button->GetText() == std::to_string(Cycle + 1) && Button->GetName() == "ApplicationTimer", "paused app click and timer");
            Check(Session.GetSimulationTick() == Tick && Number(*World, "Ran") == 0, "world advanced while parked");
            Check(HeldHeight < 50 && Falling->GetPosition().y == HeldHeight, "falling body did not remain parked");
            Menu->SetEnabled(false);
            DispatchingApplication = false;
            SimTime += 16ms;
            Session.Step();
            const double ResumedDelta = Number(*World, "FrameMs");
            std::cout << "[Lifecycle:Probe] resume_sample_ms=" << ResumedDelta
                      << " tick=" << Session.GetSimulationTick() << " timer=" << Number(*World, "Ran") << std::endl;
            Check(std::abs(ResumedDelta - 16) < .01 && Number(*World, "Ran") == 0, "first resume frame or early timer");
            SimTime += 80ms;
            Session.Step();
            Check(Number(*World, "Ran") == 1 && std::abs(Number(*World, "WaitMs") - 128) < .01, "timer count or elapsed");
            Check(Falling->GetPosition().y < HeldHeight, "physics failed to resume");
            std::cout << "[Lifecycle:Probe] cycle=" << Cycle << " parked_app_ms=60000 resumed_dt_ms="
                << ResumedDelta << " wait_elapsed_ms=" << Number(*World, "WaitMs")
                << " ran=" << Number(*World, "Ran") << '\n';
            Session.Destroy();
            World->Destroy();
            Menu->SetEnabled(true);
            Check(Previous->GetDestroyed() && Timer->Thread == nullptr, "session retirement");
        }
        // A network client Engine rejects a graph lacking the GameSession-bound player.
        bool Rejected = false;
        try {
            auto Unbound = std::make_shared<DataModel>();
            HeadlessRenderer Surface(Vector2(320, 180));
            Engine Candidate(Unbound, &Surface, {}, EngineProviderConfiguration{.AudioEnabled=false, .Mode=RuntimeMode::NetworkClient});
        } catch (const std::exception &Error) {
            Rejected = std::string(Error.what()).find("LocalPlayer") != std::string::npos;
        }
        DispatchingApplication = true;
        Click(Gui);
        Check(Rejected && Button->GetText() == "4", "failure recovery or lost menu");
        std::cout << "[Lifecycle:Probe] retired_worlds=2 unbound_client=rejected recovered_click=4\n";
        App->ShutdownRuntime();
        Presentation->Destroy();
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Lifecycle:Probe] " << Error.what() << '\n';
        return 1;
    }
}
