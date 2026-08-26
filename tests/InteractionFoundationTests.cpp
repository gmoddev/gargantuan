#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ProximityPrompt.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/TextBox.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/InteractionService.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gargantuan {
	struct InteractionServiceTestAccess {
		static void Step(InteractionService &Service, InteractionService::Clock::time_point Now) {
			Service.Step(Now);
		}
		static ObjectId Query(InteractionService &Service, const glm::vec3 &Origin) {
			Service.ProcessDirtyPrompts();
			return Service.QueryNearest(Origin).Prompt;
		}
		static std::size_t QueryConsidered(InteractionService &Service, const glm::vec3 &Origin) {
			Service.ProcessDirtyPrompts();
			return Service.QueryNearest(Origin).Considered;
		}
		static std::size_t RegisteredPromptCount(const InteractionService &Service) {
			return Service.Prompts.size();
		}
		static std::size_t LastLineOfSightRaycasts(const InteractionService &Service) {
			return Service.LastLineOfSightRaycasts;
		}
	};
}

namespace {
	using namespace gargantuan;

	int Failures = 0;

	void Check(bool Condition, std::string_view Message) {
		if (!Condition) {
			std::cerr << "[Interaction:Test] FAIL: " << Message << '\n';
			++Failures;
		}
	}

	template <typename Callback> void CheckThrows(Callback &&Value, std::string_view Message) {
		try {
			Value();
			Check(false, Message);
		} catch (const std::exception &) {}
	}

	class PresentingTestRenderer final : public BaseRenderer {
	  public:
		using BaseRenderer::Draw;
		void Draw(RenderPublicationPtr Publication) override {
			LastPublication = std::move(Publication);
		}
		void Resize(int WidthValue, int HeightValue) override {
			Width = static_cast<std::uint32_t>(std::max(1, WidthValue));
			Height = static_cast<std::uint32_t>(std::max(1, HeightValue));
		}
		void Destroy() override {}
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return {Width, Height};
		}

	  private:
		std::uint32_t Width = 800;
		std::uint32_t Height = 600;
		RenderPublicationPtr LastPublication;
	};

	struct Fixture {
		std::shared_ptr<DataModel> Game = std::make_shared<DataModel>();
		std::unique_ptr<BaseRenderer> Renderer;
		std::unique_ptr<Engine> Runtime;
		std::shared_ptr<Player> LocalPlayer;
		std::shared_ptr<KinematicCharacter> Character;
		InteractionService::Clock::time_point Now{std::chrono::seconds(1)};
		std::vector<std::pair<std::string, std::string>> Diagnostics;

		explicit Fixture(bool Present = false) {
			auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
			PlayersValue->SetDefaultControllerEnabled(false);
			PlayersValue->SetDefaultCameraEnabled(false);
			if (Present)
				Renderer = std::make_unique<PresentingTestRenderer>();
			else
				Renderer = std::make_unique<HeadlessRenderer>(Vector2(800.0f, 600.0f));
			Runtime = std::make_unique<Engine>(Game, Renderer.get(), [&](std::string Severity, std::string Message) {
				Diagnostics.emplace_back(std::move(Severity), std::move(Message));
			});
			Runtime->Script->Step();
			LocalPlayer = *Runtime->Players->GetLocalPlayer();
			Character = std::make_shared<KinematicCharacter>();
			Character->SetName("InteractionCharacter");
			Character->SetParent(Runtime->Workspace);
			LocalPlayer->SetCharacter(Character);
		}

		~Fixture() {
			if (Runtime) Runtime->Destroy();
			if (Game && !Game->GetDestroyed()) Game->Destroy();
		}

		void Advance(std::chrono::milliseconds Delta = std::chrono::milliseconds(40)) {
			Now += Delta;
			InteractionServiceTestAccess::Step(*Runtime->Interaction, Now);
		}

		std::pair<std::shared_ptr<Part>, std::shared_ptr<ProximityPrompt>>
		MakePrompt(std::string Name, glm::vec3 Position, float Distance = 4.0f, float HoldDuration = 0.0f) {
			auto PartValue = std::make_shared<Part>();
			PartValue->SetName(std::move(Name));
			PartValue->SetAnchored(true);
			PartValue->SetCFrame(CFrame(Position));
			PartValue->SetParent(Runtime->Workspace);
			auto Prompt = std::make_shared<ProximityPrompt>();
			Prompt->SetName("Prompt");
			Prompt->SetActionText("Use");
			Prompt->SetObjectText(PartValue->GetName());
			Prompt->SetMaxActivationDistance(Distance);
			Prompt->SetHoldDuration(HoldDuration);
			Prompt->SetParent(PartValue);
			return {PartValue, Prompt};
		}
	};

	void TestDiscoverySelectionAndAnchors() {
		Fixture Value;
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		auto [FirstPart, First] = Value.MakePrompt("First", {4.0f, 0.0f, 0.0f}, 4.0f);
		Value.Advance();
		Check(Value.Runtime->Interaction->GetAvailable(), "exact distance boundary remains eligible");
		Check(
			Value.Runtime->Interaction->GetActivePrompt() == std::optional(First), "registered prompt becomes active"
		);

		Value.Character->SetPosition({-0.01f, 0.0f, 0.0f});
		Value.Advance();
		Check(!Value.Runtime->Interaction->GetAvailable(), "range leave hides the candidate");
		auto ReplacementCharacter = std::make_shared<KinematicCharacter>();
		ReplacementCharacter->SetPosition({0.0f, 0.0f, 0.0f});
		ReplacementCharacter->SetParent(Value.Runtime->Workspace);
		Value.LocalPlayer->SetCharacter(ReplacementCharacter);
		Value.Character->Destroy();
		Value.Character = ReplacementCharacter;
		Value.Advance();
		Check(
			Value.Runtime->Interaction->GetActivePrompt() == std::optional(First),
			"character replacement immediately changes the authoritative interaction origin"
		);

		auto [NearPart, Near] = Value.MakePrompt("Near", {1.0f, 0.0f, 0.0f});
		Value.Advance();
		Check(Value.Runtime->Interaction->GetActivePrompt() == std::optional(Near), "nearest eligible prompt wins");

		auto [TiePart, Tie] = Value.MakePrompt("Tie", {-1.0f, 0.0f, 0.0f});
		Value.Advance();
		const auto ExpectedTie = std::min(Near->GetObjectId(), Tie->GetObjectId());
		Check(
			Value.Runtime->Interaction->GetActivePrompt()->get()->GetObjectId() == ExpectedTie,
			"equal-distance prompts use stable ObjectId ordering"
		);

		Near->SetEnabled(false);
		Tie->SetEnabled(false);
		Value.Advance();
		Check(
			Value.Runtime->Interaction->GetActivePrompt() == std::optional(First), "disabled prompts leave selection"
		);

		auto AnchorPart = std::make_shared<Part>();
		AnchorPart->SetAnchored(true);
		AnchorPart->SetCFrame(CFrame(glm::vec3(10.0f, 0.0f, 0.0f)));
		AnchorPart->SetParent(Value.Runtime->Workspace);
		auto AttachmentValue = std::make_shared<Attachment>();
		AttachmentValue->SetCFrame(CFrame(glm::vec3(-7.0f, 0.0f, 0.0f)));
		AttachmentValue->SetParent(AnchorPart);
		auto AttachedPrompt = std::make_shared<ProximityPrompt>();
		AttachedPrompt->SetMaxActivationDistance(3.0f);
		AttachedPrompt->SetParent(AttachmentValue);
		First->SetEnabled(false);
		Value.Advance();
		Check(
			Value.Runtime->Interaction->GetActivePrompt() == std::optional(AttachedPrompt),
			"Attachment local CFrame resolves through ancestor BasePart world transform"
		);

		AttachmentValue->SetCFrame(CFrame(glm::vec3(5.0f, 0.0f, 0.0f)));
		Value.Advance();
		Check(
			!Value.Runtime->Interaction->GetAvailable(), "anchor property updates move only the dirty spatial record"
		);
		AttachedPrompt->SetParent(FirstPart);
		First->SetEnabled(true);
		Value.Advance();
		Check(Value.Runtime->Interaction->GetAvailable(), "prompt reparenting rebuilds its anchor registration");
		AttachedPrompt->Destroy();
		Value.Advance();
		Check(
			InteractionServiceTestAccess::RegisteredPromptCount(*Value.Runtime->Interaction) == 3,
			"destroyed prompts retire registration and generation-safe observers"
		);
	}

	void TestActivationHoldAndReentrancy() {
		Fixture Value;
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		auto [PartValue, Prompt] = Value.MakePrompt("Instant", {1.0f, 0.0f, 0.0f});
		int Triggers = 0;
		Prompt->Triggered->Connect([&](std::shared_ptr<Player> PlayerValue) {
			Check(PlayerValue == Value.LocalPlayer, "Triggered reports the authoritative local Player");
			++Triggers;
		});
		Value.Advance();
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Check(Triggers == 1, "zero-duration activation fires on one semantic press edge");
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance();
		Check(Triggers == 1, "held/repeated input cannot retrigger before release");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();
		Value.Character->SetPosition({100.0f, 0.0f, 0.0f});
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Check(Triggers == 1, "activation performs final distance validation instead of trusting prior presentation");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance();
		Check(Triggers == 2, "release and repress permits a second validated activation");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		Prompt->SetEnabled(false);
		auto [HoldPart, Hold] = Value.MakePrompt("Hold", {1.5f, 0.0f, 0.0f}, 4.0f, 0.2f);
		int HoldTriggers = 0;
		Hold->Triggered->Connect([&](std::shared_ptr<Player>) { ++HoldTriggers; });
		Value.Advance();
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Value.Advance(std::chrono::milliseconds(100));
		Check(
			Value.Runtime->Interaction->GetHoldProgress() >= 0.49f && HoldTriggers == 0,
			"hold progress uses monotonic elapsed time"
		);
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();
		Value.Advance(std::chrono::milliseconds(300));
		Check(HoldTriggers == 0 && Value.Runtime->Interaction->GetHoldProgress() == 0.0f, "release cancels hold");

		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Value.Advance(std::chrono::milliseconds(200));
		Check(HoldTriggers == 1, "hold triggers exactly once at duration");
		Value.Advance(std::chrono::milliseconds(500));
		Check(HoldTriggers == 1, "completed hold cannot repeat while input remains down");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Value.Character->SetPosition({100.0f, 0.0f, 0.0f});
		Value.Advance(std::chrono::milliseconds(250));
		Check(HoldTriggers == 1, "leaving range cancels before hold activation");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		Value.Advance();
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Hold->SetEnabled(false);
		Value.Advance(std::chrono::milliseconds(250));
		Check(HoldTriggers == 1, "disabling during hold cancels activation");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		Hold->SetEnabled(true);
		Hold->SetHoldDuration(0.0f);
		Value.Advance();
		Hold->Triggered->Connect([Hold](std::shared_ptr<Player>) { Hold->Destroy(); });
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Check(Hold->GetDestroyed(), "Triggered callback may destroy its prompt without invalid iterator use");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		auto [DestroyedParent, DestroyedDuringHold] = Value.MakePrompt(
			"DestroyedDuringHold", {1.0f, 0.0f, 0.0f}, 4.0f, 0.2f
		);
		int DestroyedHoldTriggers = 0;
		DestroyedDuringHold->Triggered->Connect([&](std::shared_ptr<Player>) { ++DestroyedHoldTriggers; });
		Value.Advance();
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		DestroyedParent->Destroy();
		Value.Advance(std::chrono::milliseconds(250));
		Check(DestroyedHoldTriggers == 0, "destroying a prompt parent cancels an in-progress hold");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		auto [FocusPart, FocusPrompt] = Value.MakePrompt("Focus", {1.0f, 0.0f, 0.0f}, 4.0f, 0.2f);
		int FocusTriggers = 0;
		FocusPrompt->Triggered->Connect([&](std::shared_ptr<Player>) { ++FocusTriggers; });
		Value.Advance();
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		(void)Value.Runtime->ProcessEvent(FocusEvent{false});
		Value.Advance(std::chrono::milliseconds(250));
		Check(FocusTriggers == 0, "focus loss cancels an in-progress hold");
	}

	void TestSemanticInputAndTextFocus() {
		Fixture Value;
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		auto [PartValue, Prompt] = Value.MakePrompt("Input", {1.0f, 0.0f, 0.0f});
		int Triggers = 0;
		Prompt->Triggered->Connect([&](std::shared_ptr<Player>) { ++Triggers; });
		Value.Advance();

		auto Key = [](LogicalKey Logical, ButtonState State, bool Repeat = false) {
			return KeyEvent{{1}, PhysicalKey::E, Logical, KeyModifier::None, State, Repeat};
		};
		(void)Value.Runtime->ProcessEvent(Key(LogicalKey::E, ButtonState::Pressed));
		Value.Advance(std::chrono::milliseconds(1));
		(void)Value.Runtime->ProcessEvent(Key(LogicalKey::E, ButtonState::Pressed, true));
		Value.Advance();
		Check(Triggers == 1, "default E binding routes through one ActionMap press edge and ignores OS repeat");
		(void)Value.Runtime->ProcessEvent(Key(LogicalKey::E, ButtonState::Released));
		Value.Advance();

		(void)Value.Runtime->ProcessEvent(GamepadButtonEvent{{1}, GamepadButton::South, ButtonState::Pressed});
		Value.Advance(std::chrono::milliseconds(1));
		(void)Value.Runtime->ProcessEvent(GamepadButtonEvent{{1}, GamepadButton::South, ButtonState::Released});
		Value.Advance();
		Check(Triggers == 2, "normalized gamepad South maps to the same Interact semantic action");

		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Value.Game);
		auto Input = std::make_shared<TextBox>();
		Input->SetInteractable(true);
		Input->SetSize(UDim2::fromOffset(240, 44));
		Input->SetParent(Root);
		(void)Value.Runtime->Gui->Reconcile();
		Input->CaptureFocus();
		const auto Consumed = Value.Runtime->ProcessEvent(Key(LogicalKey::E, ButtonState::Pressed));
		Value.Advance(std::chrono::milliseconds(1));
		(void)Value.Runtime->ProcessEvent(Key(LogicalKey::E, ButtonState::Released));
		Value.Advance();
		Check(Consumed.Consumed && Triggers == 2, "focused TextBox consumes Interact before ActionMap activation");
		Input->ReleaseFocus();
	}

	void TestDefaultTouchPresentation() {
		Fixture Value(true);
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		auto [PartValue, Prompt] = Value.MakePrompt("TouchTarget", {1.0f, 0.0f, 0.0f}, 4.0f, 0.2f);
		Prompt->SetActionText("Open");
		Prompt->SetObjectText("Terminal");
		int Triggers = 0;
		Prompt->Triggered->Connect([&](std::shared_ptr<Player>) { ++Triggers; });
		Value.Advance();
		(void)Value.Runtime->Gui->Reconcile();
		auto Root = std::dynamic_pointer_cast<ScreenGui>(Value.Game->FindFirstChild("DefaultInteractionGui", false));
		auto Panel = Root ? std::dynamic_pointer_cast<TextButton>(Root->FindFirstChild("Prompt", true)) : nullptr;
		auto ProgressFill = Panel ? std::dynamic_pointer_cast<Frame>(Panel->FindFirstChild("Fill", true)) : nullptr;
		Check(
			Root && Panel && Panel->GetVisible() && Panel->GetText().find("Open") != std::string::npos &&
				Panel->GetText().find("Terminal") != std::string::npos,
			"shipped Luau presentation consumes the native ActionText/ObjectText snapshot"
		);
		if (!Panel || !ProgressFill) return;
		const auto Point = Panel->GetAbsolutePosition() + Vector2(30.0f, 25.0f);
		const auto Down = Value.Runtime->ProcessEvent(
			TouchPointerEvent{{77}, {Point.GetX(), Point.GetY()}, {}, TouchPointerAction::Down}
		);
		Value.Advance(std::chrono::milliseconds(1));
		Value.Advance(std::chrono::milliseconds(100));
		Check(
			ProgressFill->GetSize().X.Scale >= 0.49f && Triggers == 0,
			"default touch presentation reflects native monotonic hold progress"
		);
		(void)Value.Runtime->ProcessEvent(
			TouchPointerEvent{{77}, {Point.GetX(), Point.GetY()}, {}, TouchPointerAction::Up}
		);
		Value.Advance();
		Check(Down.Consumed && Triggers == 0, "touch release cancels without duplicate activation");
		(void)Value.Runtime->ProcessEvent(
			TouchPointerEvent{{78}, {Point.GetX(), Point.GetY()}, {}, TouchPointerAction::Down}
		);
		Value.Advance(std::chrono::milliseconds(1));
		Value.Advance(std::chrono::milliseconds(200));
		(void)Value.Runtime->ProcessEvent(
			TouchPointerEvent{{78}, {Point.GetX(), Point.GetY()}, {}, TouchPointerAction::Up}
		);
		Value.Advance();
		Check(Triggers == 1, "touch prompt completes through InteractionService exactly once");
		Value.Character->SetPosition({100.0f, 0.0f, 0.0f});
		Value.Advance();
		Check(!Panel->GetVisible(), "default presentation hides after the active prompt leaves range");
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		Value.Advance();
		Check(Panel->GetVisible(), "default presentation returns when the prompt becomes eligible again");
		Value.LocalPlayer->Destroy();
		Value.Advance();
		Check(!Panel->GetVisible(), "default presentation retires when the local Player is destroyed");
	}

	void TestValidationAndLifetimeBounds() {
		Fixture Value;
		auto [PartValue, Prompt] = Value.MakePrompt("Bounds", {1.0f, 0.0f, 0.0f});
		CheckThrows([&] { Prompt->SetMaxActivationDistance(0.0f); }, "zero distance is rejected");
		CheckThrows([&] { Prompt->SetMaxActivationDistance(65.0f); }, "oversized distance is rejected");
		CheckThrows(
			[&] { Prompt->SetMaxActivationDistance(std::numeric_limits<float>::infinity()); },
			"non-finite distance is rejected"
		);
		CheckThrows([&] { Prompt->SetHoldDuration(-0.1f); }, "negative hold duration is rejected");
		CheckThrows([&] { Prompt->SetHoldDuration(31.0f); }, "oversized hold duration is rejected");
		CheckThrows([&] { Prompt->SetActionText(std::string(65, 'x')); }, "oversized prompt text is rejected");
		Check(
			InteractionService::MaximumPlayers == 64 && InteractionService::MaximumPrompts == 16'384 &&
				InteractionService::MaximumPromptsConsideredPerQuery == InteractionService::MaximumPrompts,
			"interaction player, registration, and per-query work are hard bounded"
		);

		PartValue->SetCFrame(CFrame(glm::vec3(InteractionService::MaximumSpatialCoordinate + 1.0f, 0.0f, 0.0f)));
		Value.Character->SetPosition({InteractionService::MaximumSpatialCoordinate, 0.0f, 0.0f});
		Value.Advance();
		Check(!Value.Runtime->Interaction->GetAvailable(), "out-of-bound spatial coordinates fail closed");

		Value.LocalPlayer->Destroy();
		Value.Advance();
		Check(!Value.Runtime->Interaction->GetAvailable(), "destroyed Player retires interaction state");
		Check(
			!std::ranges::any_of(Value.Diagnostics, [](const auto &Diagnostic) { return Diagnostic.first == "Error"; }),
			"core interaction tests emitted no runtime error diagnostic"
		);
	}

	void TestRigidLineOfSight() {
		Fixture Value;
		Value.Character->SetPosition({0.0f, 0.0f, 0.0f});
		auto CharacterPart = std::make_shared<Part>();
		CharacterPart->SetName("CharacterQueryPart");
		CharacterPart->SetAnchored(true);
		CharacterPart->SetCFrame(CFrame(1.0f, 0.0f, 0.0f));
		CharacterPart->SetSize({0.5f, 0.5f, 0.5f});
		CharacterPart->SetParent(Value.Character);
		auto [TargetPart, Prompt] = Value.MakePrompt("VisibleTarget", {4.0f, 0.0f, 0.0f}, 6.0f, 0.2f);
		Prompt->SetRequiresLineOfSight(true);
		int Triggers = 0;
		Prompt->Triggered->Connect([&](std::shared_ptr<Player>) { ++Triggers; });
		Value.Advance();
		Check(
			Value.Runtime->Interaction->GetAvailable(),
			"LOS excludes the player's character and accepts the target owning Part"
		);
		Check(
			InteractionServiceTestAccess::LastLineOfSightRaycasts(*Value.Runtime->Interaction) == 1,
			"one narrowed LOS prompt produces one candidate raycast"
		);

		auto Occluder = std::make_shared<Part>();
		Occluder->SetName("Occluder");
		Occluder->SetAnchored(true);
		Occluder->SetCFrame(CFrame(2.0f, 0.0f, 0.0f));
		Occluder->SetSize({0.5f, 3.0f, 3.0f});
		Occluder->SetParent(Value.Runtime->Workspace);
		Value.Advance();
		Check(!Value.Runtime->Interaction->GetAvailable(), "rigid geometry blocks an opted-in prompt");
		Prompt->SetRequiresLineOfSight(false);
		Value.Advance();
		Check(
			Value.Runtime->Interaction->GetAvailable(), "RequiresLineOfSight false preserves distance-only interaction"
		);
		Prompt->SetRequiresLineOfSight(true);
		Occluder->SetParent(nullptr);
		Value.Advance();

		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Value.Advance(std::chrono::milliseconds(80));
		Occluder->SetParent(Value.Runtime->Workspace);
		Value.Advance(std::chrono::milliseconds(140));
		Check(
			Triggers == 0 && Value.Runtime->Interaction->GetHoldProgress() == 0.0f,
			"occluder appearing during a hold cancels before Triggered"
		);
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		Occluder->SetParent(nullptr);
		Value.Advance();
		Check(Value.Runtime->Interaction->GetAvailable(), "clearing rigid geometry restores LOS availability");
		Prompt->SetHoldDuration(0.0f);
		Occluder->SetParent(Value.Runtime->Workspace);
		Value.Runtime->Interaction->BeginActivation();
		Value.Advance(std::chrono::milliseconds(1));
		Check(Triggers == 0, "zero-time press edge revalidates LOS rather than trusting prior presentation");
		Value.Runtime->Interaction->EndActivation();
		Value.Advance();

		Prompt->SetEnabled(false);
		Occluder->SetParent(nullptr);
		auto AttachmentValue = std::make_shared<Attachment>();
		AttachmentValue->SetCFrame(CFrame(-1.0f, 0.0f, 0.0f));
		AttachmentValue->SetParent(TargetPart);
		auto AttachedPrompt = std::make_shared<ProximityPrompt>();
		AttachedPrompt->SetMaxActivationDistance(6.0f);
		AttachedPrompt->SetRequiresLineOfSight(true);
		AttachedPrompt->SetParent(AttachmentValue);
		Value.Advance();
		Check(
			Value.Runtime->Interaction->GetActivePrompt() == std::optional(AttachedPrompt),
			"prompt under Attachment raycasts to its exact resolved anchor and accepts the ancestor collider"
		);
		TargetPart->SetCFrame(CFrame(20.0f, 0.0f, 0.0f));
		Value.Advance();
		Check(!Value.Runtime->Interaction->GetAvailable(), "target movement during eligibility is revalidated");
	}

	void TestRendererUnavailable() {
		auto Game = std::make_shared<DataModel>();
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		Engine Runtime(Game, nullptr);
		Runtime.Script->Step();
		auto LocalPlayer = *Runtime.Players->GetLocalPlayer();
		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetPosition({0.0f, 0.0f, 0.0f});
		Character->SetParent(Runtime.Workspace);
		LocalPlayer->SetCharacter(Character);
		auto PartValue = std::make_shared<Part>();
		PartValue->SetAnchored(true);
		PartValue->SetCFrame(CFrame(glm::vec3(1.0f, 0.0f, 0.0f)));
		PartValue->SetParent(Runtime.Workspace);
		auto Prompt = std::make_shared<ProximityPrompt>();
		Prompt->SetParent(PartValue);
		int Triggers = 0;
		Prompt->Triggered->Connect([&](std::shared_ptr<Player>) { ++Triggers; });
		auto Now = InteractionService::Clock::time_point(std::chrono::seconds(1));
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
		Runtime.Interaction->BeginActivation();
		InteractionServiceTestAccess::Step(*Runtime.Interaction, Now + std::chrono::milliseconds(1));
		Check(
			Triggers == 1 && !Runtime.Interaction->GetDefaultPresentationEnabled() &&
				!Game->FindFirstChild("DefaultInteractionGui", false),
			"interaction authority remains functional when no renderer or GUI presentation is available"
		);
		Runtime.Destroy();
		Game->Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Error) {
		std::cerr << "[Interaction:Test] Schema bootstrap failed: " << Error.what() << '\n';
		return 1;
	}
	TestDiscoverySelectionAndAnchors();
	TestActivationHoldAndReentrancy();
	TestSemanticInputAndTextFocus();
	TestDefaultTouchPresentation();
	TestValidationAndLifetimeBounds();
	TestRigidLineOfSight();
	TestRendererUnavailable();
	if (Failures == 0) std::cout << "[Interaction:Test] All interaction foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}
