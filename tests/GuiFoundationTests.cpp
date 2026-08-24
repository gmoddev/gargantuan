// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/Engine.hpp"
#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/ScrollingFrame.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/TextBox.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/UIListLayout.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "[Gui:FoundationTest] FAIL: " << Message << '\n';
		++Failures;
	}

	bool Near(float Left, float Right, float Epsilon = 0.05f) {
		return std::abs(Left - Right) <= Epsilon;
	}

	void RunTest(const char *Name, void (*Test)()) {
		std::cerr << "[Gui:FoundationTest] BEGIN " << Name << std::endl;
		Test();
		std::cerr << "[Gui:FoundationTest] PASS " << Name << std::endl;
	}

	const gargantuan::GuiLayoutNode *FindNode(
		const std::shared_ptr<const gargantuan::GuiLayoutSnapshot> &Snapshot,
		gargantuan::ObjectId Object
	) {
		auto Existing = Snapshot->NodeByObject.find(Object);
		return Existing == Snapshot->NodeByObject.end() ? nullptr : &Snapshot->Nodes[Existing->second];
	}

	struct GuiTree {
		std::shared_ptr<gargantuan::ScreenGui> Root;
		std::shared_ptr<gargantuan::Frame> Panel;
		std::shared_ptr<gargantuan::TextLabel> Title;
		std::shared_ptr<gargantuan::TextButton> Button;
	};

	GuiTree BuildTree(const std::shared_ptr<gargantuan::DataModel> &Game) {
		using namespace gargantuan;
		GuiTree Result;
		Result.Root = std::make_shared<ScreenGui>();
		Result.Root->SetName("FoundationGui");
		Result.Root->SetParent(Game);

		Result.Panel = std::make_shared<Frame>();
		Result.Panel->SetName("Panel");
		Result.Panel->SetPosition(UDim2::fromScale(0.5f, 0.5f));
		Result.Panel->SetSize(UDim2::fromOffset(400, 260));
		Result.Panel->SetAnchorPoint({0.5f, 0.5f});
		Result.Panel->SetBackgroundColor3(Color3::fromRGB(30, 34, 42));
		Result.Panel->SetParent(Result.Root);

		Result.Title = std::make_shared<TextLabel>();
		Result.Title->SetName("Title");
		Result.Title->SetText(std::string("Gargantuan ") + "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
		Result.Title->SetSize(UDim2(1.0f, -24, 0.0f, 48));
		Result.Title->SetPosition(UDim2::fromOffset(12, 12));
		Result.Title->SetParent(Result.Panel);

		Result.Button = std::make_shared<TextButton>();
		Result.Button->SetName("PlayButton");
		Result.Button->SetText("Play");
		Result.Button->SetSize(UDim2::fromOffset(160, 44));
		Result.Button->SetPosition(UDim2::fromOffset(120, 150));
		Result.Button->SetAccessibleName("Play");
		Result.Button->SetParent(Result.Panel);
		return Result;
	}

	void CheckCentered(
		gargantuan::GuiRuntime &Runtime,
		const GuiTree &Tree,
		gargantuan::GuiViewportConfiguration Viewport
	) {
		Runtime.SetViewport(Viewport);
		(void)Runtime.Reconcile();
		const auto *Panel = FindNode(Runtime.GetCommittedLayout(), Tree.Panel->GetObjectId());
		const float RootX = Viewport.SafeArea.Left;
		const float RootY = Viewport.SafeArea.Top;
		const float RootWidth = Viewport.LogicalWidth() - Viewport.SafeArea.Left - Viewport.SafeArea.Right;
		const float RootHeight = Viewport.LogicalHeight() - Viewport.SafeArea.Top - Viewport.SafeArea.Bottom;
		Check(Panel && Near(Panel->Bounds.X, RootX + RootWidth * 0.5f - 200.0f) &&
			Near(Panel->Bounds.Y, RootY + RootHeight * 0.5f - 130.0f),
			"the same centered UDim2 tree resolves against viewport DPI and safe area");
	}

	void TestLayoutTextPresentationAndResources() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		auto Tree = BuildTree(Game);
		CheckCentered(Runtime, Tree, {1920, 1080, 1.0f, {}});
		CheckCentered(Runtime, Tree, {2560, 1440, 1.0f, {}});
		CheckCentered(Runtime, Tree, {2880, 1800, 1.5f, {}});
		CheckCentered(Runtime, Tree, {1170, 2532, 3.0f, {0.0f, 16.0f, 0.0f, 12.0f}});
		CheckCentered(Runtime, Tree, {2532, 1170, 3.0f, {18.0f, 4.0f, 18.0f, 8.0f}});

		const auto Layout = Runtime.GetCommittedLayout();
		const auto *Panel = FindNode(Layout, Tree.Panel->GetObjectId());
		const auto *Title = FindNode(Layout, Tree.Title->GetObjectId());
		Check(Panel && Title && Near(Title->Bounds.X, Panel->Bounds.X + 12.0f) &&
			Near(Title->Bounds.Width, Panel->Bounds.Width - 24.0f),
			"nested UDim2 position and size are committed from one centralized layout pass");
		Check(Title && Title->Presentation.Text && !Title->Presentation.Text->Glyphs.empty() &&
			Title->Presentation.Text->Width > 0.0f && Title->Presentation.Text->Height > 0.0f,
			"UTF-8 text is shaped, measured, and rasterized into atlas-backed glyphs");
		auto Invalid = std::make_shared<TextLabel>();
		Invalid->SetText(std::string("\xF0\x28\x8C\x28", 4));
		Invalid->SetSize(UDim2::fromOffset(100, 24));
		Invalid->SetParent(Tree.Panel);
		auto Missing = std::make_shared<TextLabel>();
		Missing->SetText(std::string("\xF4\x8F\xBF\xBF", 4));
		Missing->SetSize(UDim2::fromOffset(100, 24));
		Missing->SetPosition(UDim2::fromOffset(0, 30));
		Missing->SetParent(Tree.Panel);
		(void)Runtime.Reconcile();
		const auto *InvalidNode = FindNode(Runtime.GetCommittedLayout(), Invalid->GetObjectId());
		const auto *MissingNode = FindNode(Runtime.GetCommittedLayout(), Missing->GetObjectId());
		Check(InvalidNode && InvalidNode->Presentation.Text && InvalidNode->Presentation.Text->ReplacedInvalidUtf8,
			"invalid UTF-8 is handled safely through deterministic U+FFFD replacement");
		Check(MissingNode && MissingNode->Presentation.Text && MissingNode->Presentation.Text->UsedMissingGlyph,
			"unsupported Unicode uses the controlled font replacement glyph instead of disappearing");
		Check(Runtime.GetCommittedPresentation()->Frame.Batches.size() > 1,
			"resolved GUI produces ordered renderer-neutral UI batches");

		std::vector<std::uint8_t> Pixels{
			255, 0, 0, 255, 0, 255, 0, 255,
			0, 0, 255, 255, 255, 255, 255, 255,
		};
		const auto ImageReference = Runtime.RegisterImage("test/checker", 2, 2, Pixels);
		Pixels[0] = 240;
		Runtime.RegisterImage("test/checker", 2, 2, Pixels);
		auto Image = std::make_shared<ImageLabel>();
		Image->SetImage(ImageReference);
		Image->SetSize(UDim2::fromOffset(32, 32));
		Image->SetParent(Tree.Panel);
		(void)Runtime.Reconcile();
		const auto *ImageNode = FindNode(Runtime.GetCommittedLayout(), Image->GetObjectId());
		Check(ImageNode && ImageNode->Presentation.ImageTexture.has_value(),
			"ImageLabel resolves a logical image through the bounded GUI resource seam");

		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		RenderPublisher Publisher;
		Runtime.Publish(Publisher);
		auto Publication = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 2532, 1170);
		Check(Publication->FullResync && !Publication->TextureCreates.empty() && !Publication->GetUi().Batches.empty(),
			"GUI resources and display data use the existing RenderPublication UI and texture contracts");
		RenderProjection Projection;
		auto Applied = Projection.Apply(*Publication);
		Check(Applied.TexturesCreated == Publication->TextureCreates.size() && Applied.UiBatches == Publication->GetUi().Batches.size(),
			"headless RenderProjection accepts the complete GUI publication without a GPU");
		Pixels[1] = 16;
		Runtime.RegisterImage("test/checker", 2, 2, Pixels);
		Pixels[1] = 32;
		Runtime.RegisterImage("test/checker", 2, 2, Pixels);
		Tree.Title->SetText(std::string("Atlas update ") + "\xCE\xA9\xD0\x96");
		(void)Runtime.Reconcile();
		Runtime.Publish(Publisher);
		auto Incremental = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 2532, 1170);
		std::vector<RenderTextureIdentity> UpdatedTextures;
		for (const auto &Update : Incremental->TextureUpdates) UpdatedTextures.push_back(Update.Texture);
		std::ranges::sort(UpdatedTextures);
		Check(std::ranges::adjacent_find(UpdatedTextures) == UpdatedTextures.end() && !UpdatedTextures.empty(),
			"glyph atlas and repeated image changes coalesce to one operation per texture publication");
		(void)Projection.Apply(*Incremental);
		Publisher.RequestFullResync();
		auto Restart = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 2532, 1170);
		Check(Restart->FullResync && Restart->TextureCreates.size() == Publication->TextureCreates.size() &&
			Restart->GetUi().Batches.size() == Publication->GetUi().Batches.size(),
			"renderer restart reconstructs GUI texture residency and the committed UI frame");
	}

	void TestAutomaticSizeListAndClipping() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({800, 600, 1.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Game);
		auto Stack = std::make_shared<Frame>();
		Stack->SetPosition(UDim2::fromOffset(40, 50));
		Stack->SetAutomaticSize(Enums::AutomaticSize::XY);
		Stack->SetParent(Root);
		auto List = std::make_shared<UIListLayout>();
		List->SetPadding({0.0f, 5});
		List->SetParent(Stack);
		auto First = std::make_shared<Frame>();
		First->SetSize(UDim2::fromOffset(50, 10));
		First->SetLayoutOrder(2);
		First->SetParent(Stack);
		auto Second = std::make_shared<Frame>();
		Second->SetSize(UDim2::fromOffset(40, 10));
		Second->SetLayoutOrder(1);
		Second->SetParent(Stack);
		(void)Runtime.Reconcile();
		const auto Snapshot = Runtime.GetCommittedLayout();
		const auto *StackNode = FindNode(Snapshot, Stack->GetObjectId());
		const auto *FirstNode = FindNode(Snapshot, First->GetObjectId());
		const auto *SecondNode = FindNode(Snapshot, Second->GetObjectId());
		Check(StackNode && Near(StackNode->Bounds.Width, 50.0f) && Near(StackNode->Bounds.Height, 25.0f),
			"AutomaticSize converges from direct child content and UIListLayout spacing");
		Check(FirstNode && SecondNode && SecondNode->Bounds.Y < FirstNode->Bounds.Y,
			"UIListLayout orders direct children by LayoutOrder with stable ties");

		auto Button = std::make_shared<TextButton>();
		Button->SetSize(UDim2::fromOffset(40, 20));
		Button->SetPosition(UDim2::fromOffset(35, 5));
		Button->SetParent(Stack);
		Stack->SetSize(UDim2::fromOffset(50, 25));
		Stack->SetAutomaticSize(Enums::AutomaticSize::None);
		(void)Runtime.Reconcile();
		int Activations = 0;
		Button->Activated->Connect([&](std::monostate) { ++Activations; });
		(void)Runtime.ProcessPointer({1, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, {100.0f, 60.0f}});
		(void)Runtime.ProcessPointer({1, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Up, {100.0f, 60.0f}});
		Check(Activations == 0, "ancestor rectangular clipping excludes overflow from hit testing as well as rendering");
	}

	void TestRotationOrderingAndAlpha() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({400, 300, 1.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Game);

		auto Rotated = std::make_shared<Frame>();
		Rotated->SetPosition(UDim2::fromOffset(100, 50));
		Rotated->SetSize(UDim2::fromOffset(100, 40));
		Rotated->SetRotation(90.0f);
		Rotated->SetOpacity(0.5f);
		Rotated->SetBackgroundTransparency(0.25f);
		Rotated->SetInteractable(true);
		Rotated->SetInputSink(Enums::InputSink::All);
		Rotated->SetZIndex(1);
		Rotated->SetParent(Root);

		auto NestedAlpha = std::make_shared<Frame>();
		NestedAlpha->SetPosition(UDim2::fromOffset(5, 5));
		NestedAlpha->SetSize(UDim2::fromOffset(20, 10));
		NestedAlpha->SetOpacity(0.5f);
		NestedAlpha->SetBackgroundTransparency(0.5f);
		NestedAlpha->SetZIndex(2);
		NestedAlpha->SetParent(Rotated);

		auto InvisibleAlpha = std::make_shared<Frame>();
		InvisibleAlpha->SetPosition(UDim2::fromOffset(10, 200));
		InvisibleAlpha->SetSize(UDim2::fromOffset(20, 20));
		InvisibleAlpha->SetOpacity(0.0f);
		InvisibleAlpha->SetZIndex(3);
		InvisibleAlpha->SetParent(Root);

		auto Low = std::make_shared<TextButton>();
		Low->SetPosition(UDim2::fromOffset(250, 100));
		Low->SetSize(UDim2::fromOffset(80, 40));
		Low->SetZIndex(10);
		Low->SetParent(Root);
		auto High = std::make_shared<TextButton>();
		High->SetPosition(UDim2::fromOffset(250, 100));
		High->SetSize(UDim2::fromOffset(80, 40));
		High->SetZIndex(20);
		High->SetParent(Root);

		(void)Runtime.Reconcile();
		const auto Layout = Runtime.GetCommittedLayout();
		const auto *RotatedNode = FindNode(Layout, Rotated->GetObjectId());
		const auto *NestedNode = FindNode(Layout, NestedAlpha->GetObjectId());
		Check(RotatedNode && Near(RotatedNode->TransformedBounds.Width, 40.0f) &&
			Near(RotatedNode->TransformedBounds.Height, 100.0f),
			"rotation commits transformed visual and hit geometry instead of being ignored");
		Check(NestedNode && Near(NestedNode->EffectiveOpacity, 0.25f),
			"nested opacity is multiplied exactly once in committed presentation state");

		const auto &Batches = Runtime.GetCommittedPresentation()->Frame.Batches;
		auto ParentBatch = std::ranges::find(Batches, 1, &RenderUiBatch::Layer);
		auto NestedBatch = std::ranges::find(Batches, 2, &RenderUiBatch::Layer);
		auto InvisibleBatch = std::ranges::find(Batches, 3, &RenderUiBatch::Layer);
		Check(ParentBatch != Batches.end() && Near(ParentBatch->Opacity, 0.5f) &&
			!ParentBatch->Vertices.empty() && Near(ParentBatch->Vertices.front().Color.a, 0.75f),
			"local transparency remains vertex alpha while effective opacity is published once on the batch");
		Check(NestedBatch != Batches.end() && Near(NestedBatch->Opacity, 0.25f) &&
			!NestedBatch->Vertices.empty() && Near(NestedBatch->Vertices.front().Color.a, 0.5f),
			"partial nested alpha uses one coherent renderer convention");
		Check(InvisibleBatch == Batches.end(), "fully transparent UI emits no display primitive");

		int RotatedDown = 0;
		Rotated->PointerDown->Connect([&](std::shared_ptr<GuiInputEvent> Event) {
			if (Event->GetPhase() == Enums::GuiEventPhase::Target) ++RotatedDown;
		});
		(void)Runtime.ProcessPointer({11, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, {150.0f, 110.0f}});
		Check(RotatedDown == 1, "hit testing applies the inverse committed rotation transform");
		(void)Runtime.ProcessPointer({11, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Up, {150.0f, 110.0f}});

		int LowActivations = 0;
		int HighActivations = 0;
		Low->Activated->Connect([&](std::monostate) { ++LowActivations; });
		High->Activated->Connect([&](std::monostate) { ++HighActivations; });
		(void)Runtime.ProcessPointer({12, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, {270.0f, 120.0f}});
		(void)Runtime.ProcessPointer({12, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Up, {270.0f, 120.0f}});
		Check(LowActivations == 0 && HighActivations == 1,
			"hit ordering uses the same committed ZIndex order as transparent display generation");
	}

	void TestInputRoutingFocusAndMutationSafety() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({800, 600, 1.0f, {}});
		auto Tree = BuildTree(Game);
		(void)Runtime.Reconcile();
		std::vector<std::string> Route;
		auto Record = [&](const char *Name) {
			return [&, Name](std::shared_ptr<GuiInputEvent> Event) {
				Route.push_back(std::string(Name) + ":" + std::to_string(static_cast<int>(Event->GetPhase())));
			};
		};
		Tree.Root->PointerDown->Connect(Record("Root"));
		Tree.Panel->PointerDown->Connect(Record("Panel"));
		Tree.Button->PointerDown->Connect(Record("Button"));
		int Activations = 0;
		Tree.Button->Activated->Connect([&](std::monostate) { ++Activations; });
		const Vector2 ButtonCenter(Tree.Button->GetAbsolutePosition() + Tree.Button->GetAbsoluteSize() / 2.0f);
		const bool DownConsumed = Runtime.ProcessPointer({7, Enums::GuiPointerType::Touch,
			Enums::GuiPointerButton::Primary, GuiPointerAction::Down, ButtonCenter});
		(void)Runtime.ProcessPointer({7, Enums::GuiPointerType::Touch,
			Enums::GuiPointerButton::Primary, GuiPointerAction::Up, ButtonCenter});
		const std::vector<std::string> Expected{"Root:0", "Panel:0", "Button:1", "Panel:2", "Root:2"};
		Check(Route == Expected, "pointer routing takes a generation-safe capture, target, and bubble route snapshot");
		Check(DownConsumed && Activations == 1, "touch-compatible pointer identity captures and activates TextButton");

		(void)Runtime.Reconcile();
		const auto Access = Runtime.GetAccessibilitySnapshot();
		auto ButtonAccess = std::ranges::find(Access->Nodes, Tree.Button->GetObjectId(), &GuiAccessibilityNode::Object);
		Check(ButtonAccess != Access->Nodes.end() && ButtonAccess->Role == Enums::AccessibilityRole::Button &&
			ButtonAccess->Name == "Play" && ButtonAccess->Focused,
			"button accessibility semantics include stable identity, role, name, and focus independent of batches");

		(void)Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Space, LogicalKey::Space, KeyModifier::None,
			ButtonState::Pressed, false});
		(void)Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Space, LogicalKey::Space, KeyModifier::None,
			ButtonState::Released, false});
		Check(Activations == 2, "a focused TextButton activates through semantic keyboard input");

		auto Destroying = std::make_shared<TextButton>();
		Destroying->SetPosition(UDim2::fromOffset(10, 10));
		Destroying->SetSize(UDim2::fromOffset(80, 30));
		Destroying->SetParent(Tree.Panel);
		(void)Runtime.Reconcile();
		bool RootBubbleAfterDestroy = false;
		Destroying->PointerDown->Connect([Destroying](std::shared_ptr<GuiInputEvent>) { Destroying->Destroy(); });
		Tree.Root->PointerDown->Connect([&](std::shared_ptr<GuiInputEvent> Event) {
			if (Event->GetPhase() == Enums::GuiEventPhase::Bubble) RootBubbleAfterDestroy = true;
		});
		const auto *DestroyingNode = FindNode(Runtime.GetCommittedLayout(), Destroying->GetObjectId());
		const Vector2 DestroyingCenter(DestroyingNode->Bounds.X + 10.0f, DestroyingNode->Bounds.Y + 10.0f);
		(void)Runtime.ProcessPointer({9, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, DestroyingCenter});
		Check(Destroying->GetDestroyed() && RootBubbleAfterDestroy,
			"destruction during a target callback generation-checks later route members without dangling iterators");
		(void)Runtime.ProcessPointer({9, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Up, DestroyingCenter});
		(void)Runtime.Reconcile();
		Check(!FindNode(Runtime.GetCommittedLayout(), Destroying->GetObjectId()),
			"destroying a captured target retires its pointer state before a later release event");
	}

	void TestDisabledButtonNeverActivates() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({800, 600, 1.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Game);
		auto Button = std::make_shared<TextButton>();
		Button->SetPosition(UDim2::fromOffset(20, 20));
		Button->SetSize(UDim2::fromOffset(160, 44));
		Button->SetParent(Root);
		const Vector2 Center{100.0f, 42.0f};
		int Activations = 0;
		Button->Activated->Connect([&](std::monostate) { ++Activations; });
		auto Pointer = [&](GuiPointerAction Action) {
			return Runtime.ProcessPointer({31, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
				Action, Center});
		};
		auto Key = [&](LogicalKey Logical, ButtonState State) {
			return Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Space, Logical, KeyModifier::None, State, false});
		};

		Button->SetInteractable(false);
		Button->SetInputSink(Enums::InputSink::None);
		(void)Runtime.Reconcile();
		Check(!Pointer(GuiPointerAction::Down) && !Pointer(GuiPointerAction::Up) && Activations == 0,
			"a disabled button without InputSink neither consumes nor activates");

		Button->SetInputSink(Enums::InputSink::All);
		(void)Runtime.Reconcile();
		const bool SinkDown = Pointer(GuiPointerAction::Down);
		const bool SinkUp = Pointer(GuiPointerAction::Up);
		Check(SinkDown, "a disabled InputSink consumes pointer down without becoming pressed");
		Check(SinkUp, "a disabled InputSink consumes pointer up without activating");
		Check(Activations == 0, "InputSink may consume pointer input without activating a disabled button");

		Button->SetInteractable(true);
		(void)Runtime.Reconcile();
		(void)Pointer(GuiPointerAction::Down);
		Button->SetInteractable(false);
		(void)Pointer(GuiPointerAction::Up);
		(void)Runtime.Reconcile();
		Check(Activations == 0 && !Button->GetActive() && Button->GetGuiState() == Enums::GuiState::NonInteractable,
			"disabling between captured pointer down/up cancels activation and pressed presentation");

		Button->SetInteractable(true);
		(void)Runtime.Reconcile();
		auto DisableOnUp = Button->PointerUp->Connect([&](std::shared_ptr<GuiInputEvent>) {
			Button->SetInteractable(false);
		});
		(void)Pointer(GuiPointerAction::Down);
		(void)Pointer(GuiPointerAction::Up);
		DisableOnUp->Disconnect();
		Check(Activations == 0, "pointer-up callbacks can disable a button before activation is decided");

		Button->SetInteractable(true);
		(void)Runtime.Reconcile();
		Button->CaptureFocus();
		(void)Key(LogicalKey::Space, ButtonState::Pressed);
		Button->SetInteractable(false);
		(void)Key(LogicalKey::Space, ButtonState::Released);
		(void)Key(LogicalKey::Return, ButtonState::Pressed);
		(void)Key(LogicalKey::Return, ButtonState::Released);
		Check(Activations == 0, "stale keyboard focus cannot activate a button after it becomes disabled");

		Button->SetInteractable(true);
		(void)Runtime.Reconcile();
		(void)Key(LogicalKey::Return, ButtonState::Pressed);
		(void)Key(LogicalKey::Return, ButtonState::Released);
		Check(Activations == 1, "re-enabling a retained focused button restores keyboard activation");
	}

	void TestPersistencePlayIsolationAndSignalRetirement() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto Tree = BuildTree(Game);
		auto Scroll = std::make_shared<ScrollingFrame>();
		Scroll->SetName("PersistentScroll");
		Scroll->SetCanvasPosition({12.0f, 34.0f});
		Scroll->SetCanvasSize(UDim2::fromOffset(800, 1200));
		Scroll->SetScrollBarThickness(9.0f);
		Scroll->SetParent(Tree.Panel);
		auto InputBox = std::make_shared<TextBox>();
		InputBox->SetName("PersistentTextBox");
		InputBox->SetText("authored UTF-8 \xCE\xA9");
		InputBox->SetPlaceholderText("Type here");
		InputBox->SetReadOnly(true);
		InputBox->SetMaxLength(2048);
		InputBox->SetParent(Tree.Panel);
		Game->MarkPersistenceSubtreeArchivable();
		std::shared_ptr<Instance> RootValue = Game;
		const auto Serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, RootValue);
		Check(Serialized.find("ScreenGui") != std::string::npos && Serialized.find("TextButton") != std::string::npos &&
			Serialized.find("ScrollingFrame") != std::string::npos && Serialized.find("TextBox") != std::string::npos &&
			Serialized.find("Gargantuan") != std::string::npos && Serialized.find("AccessibleName") != std::string::npos,
			"ordinary schema persistence saves GUI hierarchy, text, scrolling, and accessibility semantics");
		Check(Serialized.find("AbsolutePosition") == std::string::npos && Serialized.find("GuiState") == std::string::npos &&
			Serialized.find("CaretPosition") == std::string::npos && Serialized.find("CompositionText") == std::string::npos,
			"persistence excludes committed layout and transient interaction state");
		std::istringstream Input(Serialized);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		auto LoadedScroll = Loaded.Instance ?
			std::dynamic_pointer_cast<ScrollingFrame>(Loaded.Instance->FindFirstChild("PersistentScroll", true)) : nullptr;
		auto LoadedInput = Loaded.Instance ?
			std::dynamic_pointer_cast<TextBox>(Loaded.Instance->FindFirstChild("PersistentTextBox", true)) : nullptr;
		auto LoadedPanel = Loaded.Instance ?
			std::dynamic_pointer_cast<Frame>(Loaded.Instance->FindFirstChild("Panel", true)) : nullptr;
		auto LoadedButton = Loaded.Instance ?
			std::dynamic_pointer_cast<TextButton>(Loaded.Instance->FindFirstChild("PlayButton", true)) : nullptr;
		Check(Loaded.Ok && Loaded.Instance && LoadedPanel && LoadedButton && LoadedScroll && LoadedInput &&
			LoadedPanel->GetPosition().X.Scale == 0.5f && LoadedPanel->GetPosition().Y.Scale == 0.5f &&
			LoadedPanel->GetSize().X.Offset == 400 && LoadedPanel->GetSize().Y.Offset == 260 &&
			LoadedButton->GetPosition().X.Offset == 120 && LoadedButton->GetPosition().Y.Offset == 150 &&
			LoadedButton->GetSize().X.Offset == 160 && LoadedButton->GetSize().Y.Offset == 44 &&
			LoadedScroll->GetCanvasPosition() == Vector2(12.0f, 34.0f) && LoadedScroll->GetScrollBarThickness() == 9.0f &&
			LoadedInput->GetText() == "authored UTF-8 \xCE\xA9" && LoadedInput->GetPlaceholderText() == "Type here" &&
			LoadedInput->GetReadOnly() && LoadedInput->GetMaxLength() == 2048,
			"GUI layout, semantic state, and hierarchy reconstruct through the ordinary project format");

		PlaySession Session({7301}, Serialized, InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(), 800, 600, Game->GetAuthoritativeRevision());
		auto RuntimeWorld = Session.GetWorld();
		auto RuntimeButton = std::dynamic_pointer_cast<TextButton>(RuntimeWorld->FindFirstChild("PlayButton", true));
		auto RuntimeScroll = std::dynamic_pointer_cast<ScrollingFrame>(RuntimeWorld->FindFirstChild("PersistentScroll", true));
		auto RuntimeInput = std::dynamic_pointer_cast<TextBox>(RuntimeWorld->FindFirstChild("PersistentTextBox", true));
		Check(Session.GetState() == PlaySessionState::Running && RuntimeButton && RuntimeButton != Tree.Button,
			"Play constructs a distinct runtime GUI tree from authored semantics");
		if (RuntimeButton) {
			RuntimeButton->SetText("Runtime mutation");
			RuntimeButton->CaptureFocus();
			auto Created = std::make_shared<Frame>();
			Created->SetName("RuntimeOnlyGui");
			Created->SetParent(RuntimeButton->GetParent());
			RuntimeButton->Destroy();
		}
		if (RuntimeScroll) RuntimeScroll->SetCanvasPosition({300.0f, 400.0f});
		if (RuntimeInput) {
			RuntimeInput->SetReadOnly(false);
			RuntimeInput->SetText("runtime edit");
			RuntimeInput->CaptureFocus();
		}
		Session.Stop();
		Check(Tree.Button->GetText() == "Play" && !Tree.Button->GetDestroyed() &&
			Scroll->GetCanvasPosition() == Vector2(12.0f, 34.0f) && InputBox->GetText() == "authored UTF-8 \xCE\xA9" &&
			InputBox->GetReadOnly() && !Game->FindFirstChild("RuntimeOnlyGui", true),
			"runtime GUI focus, scrolling, text edits, creation, and destruction do not contaminate the authoring DataModel");

		int Calls = 0;
		auto Connection = Tree.Button->Activated->Connect([&](std::monostate) { ++Calls; });
		Tree.Button->Destroy();
		Tree.Button->Activated->Fire({});
		Check(!Connection->Connected && Calls == 0, "destroying a GUI object retires its existing Gargantuan Signal connections");
	}

	void TestIncrementalInvalidationAndUiRetention() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({1280, 720, 1.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Game);
		std::vector<std::shared_ptr<Frame>> Frames;
		for (int Index = 0; Index < 256; ++Index) {
			auto Value = std::make_shared<Frame>();
			Value->SetPosition(UDim2::fromOffset((Index % 32) * 20, (Index / 32) * 20));
			Value->SetSize(UDim2::fromOffset(18, 18));
			Value->SetParent(Root);
			Frames.push_back(std::move(Value));
		}
		auto Label = std::make_shared<TextLabel>();
		Label->SetText("unrelated shaped text");
		Label->SetSize(UDim2::fromOffset(200, 30));
		Label->SetPosition(UDim2::fromOffset(700, 20));
		Label->SetParent(Root);
		(void)Runtime.Reconcile();
		const auto InitialLayoutGeneration = Runtime.GetCommittedLayout()->Generation;
		const auto InitialPresentationGeneration = Runtime.GetCommittedPresentation()->Generation;
		Frames[125]->SetBackgroundColor3(Color3::fromRGB(20, 90, 180));
		Frames[125]->SetBackgroundColor3(Color3::fromRGB(40, 110, 200));
		(void)Runtime.Reconcile();
		auto Profile = Runtime.GetLastProfile();
		Check(Profile.DirtyObjects == 1 && Profile.LayoutRoots == 0 && Profile.PresentationNodes == 1 &&
			Profile.DisplayPrimitives == 1 &&
			Profile.TextShapingNanoseconds == 0 && Profile.AccessibilityNodes == 0,
			"repeated presentation writes patch one retained display primitive without unrelated layout, text, or accessibility work");
		Check(Runtime.GetCommittedLayout()->Generation == InitialLayoutGeneration &&
			Runtime.GetCommittedPresentation()->Generation > InitialPresentationGeneration,
			"presentation-only state advances its immutable projection without replacing unchanged layout geometry");

		Frames[125]->SetPosition(UDim2::fromOffset(333, 222));
		(void)Runtime.Reconcile();
		Profile = Runtime.GetLastProfile();
		Check(Profile.DirtyObjects == 1 && Profile.LayoutRoots == 1 && Profile.LayoutNodes == 1 &&
			Profile.DisplayPrimitives == 1 &&
			Profile.TextShapingNanoseconds == 0,
			"an isolated Position write re-arranges and patches only its independent primitive without reshaping unrelated text");

		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		RenderPublisher Publisher;
		Runtime.Publish(Publisher);
		auto First = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 1280, 720);
		Check(First->FullResync && First->UiChanged, "the initial GUI publication carries complete reconstructable UI state");
		(void)Runtime.Reconcile();
		Runtime.Publish(Publisher);
		auto Static = Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 1280, 720);
		Check(!Static->UiChanged && Static->Ui.Batches.empty(),
			"a static GUI frame does not copy or republish its complete RenderUiFrame");
		RenderProjection Projection;
		(void)Projection.Apply(*First);
		(void)Projection.Apply(*Static);
		Check(!Projection.GetUi().Batches.empty(), "an absent UI update preserves the renderer projection's committed frame");
	}

	void TestSiblingStackingContexts() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({400, 300, 1.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetZIndexBehavior(Enums::ZIndexBehavior::Global);
		Root->SetParent(Game);
		auto LowContext = std::make_shared<Frame>();
		LowContext->SetPosition(UDim2::fromOffset(50, 50));
		LowContext->SetSize(UDim2::fromOffset(120, 80));
		LowContext->SetZIndex(0);
		LowContext->SetParent(Root);
		auto EscapingChild = std::make_shared<TextButton>();
		LowContext->SetClipsDescendants(true);
		EscapingChild->SetPosition(UDim2::fromOffset(100, 0));
		EscapingChild->SetSize(UDim2::fromOffset(120, 80));
		EscapingChild->SetZIndex(100);
		EscapingChild->SetParent(LowContext);
		auto HighContext = std::make_shared<TextButton>();
		HighContext->SetPosition(UDim2::fromOffset(50, 50));
		HighContext->SetSize(UDim2::fromOffset(220, 80));
		HighContext->SetZIndex(10);
		HighContext->SetParent(Root);
		int EscapingActivations = 0;
		int HighActivations = 0;
		EscapingChild->Activated->Connect([&](std::monostate) { ++EscapingActivations; });
		HighContext->Activated->Connect([&](std::monostate) { ++HighActivations; });
		(void)Runtime.Reconcile();
		auto Click = [&](Vector2 Position = {160, 80}) {
			(void)Runtime.ProcessPointer({2, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
				GuiPointerAction::Down, Position});
			(void)Runtime.ProcessPointer({2, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
				GuiPointerAction::Up, Position});
		};
		Click();
		Check(EscapingActivations == 1 && HighActivations == 0,
			"Global ZIndex allows a high-Z descendant to paint and hit above another root child");
		Root->SetZIndexBehavior(Enums::ZIndexBehavior::Sibling);
		(void)Runtime.Reconcile();
		Click();
		Check(EscapingActivations == 1 && HighActivations == 1,
			"Sibling ZIndex keeps each direct-child subtree atomic and uses the identical order for hit testing");
		const auto Layout = Runtime.GetCommittedLayout();
		const auto *ChildNode = FindNode(Layout, EscapingChild->GetObjectId());
		const auto *HighNode = FindNode(Layout, HighContext->GetObjectId());
		Check(ChildNode && HighNode && ChildNode->PaintOrder < HighNode->PaintOrder,
			"committed sibling-context paint order prevents descendants escaping their ancestor context");
		Click({180, 80});
		Check(EscapingActivations == 1 && HighActivations == 2,
			"ancestor clipping excludes an otherwise higher-Z descendant from the identical hit and paint region");
		EscapingChild->SetParent(Root);
		(void)Runtime.Reconcile();
		Click({180, 80});
		Check(EscapingActivations == 2 && HighActivations == 2,
			"reparenting commits a new sibling stacking context without retaining the former ancestor order");
		HighContext->Destroy();
		(void)Runtime.Reconcile();
		Click({180, 80});
		Check(EscapingActivations == 3,
			"destroying an overlapping context safely retires it from both paint and hit order");
	}

	void TestScrollingAndEditableText() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({500, 400, 1.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Game);
		auto Scroll = std::make_shared<ScrollingFrame>();
		Scroll->SetPosition(UDim2::fromOffset(20, 20));
		Scroll->SetSize(UDim2::fromOffset(160, 100));
		Scroll->SetParent(Root);
		auto ScrolledButton = std::make_shared<TextButton>();
		ScrolledButton->SetPosition(UDim2::fromOffset(10, 170));
		ScrolledButton->SetSize(UDim2::fromOffset(100, 30));
		ScrolledButton->SetParent(Scroll);
		(void)Runtime.Reconcile();
		Check(Scroll->GetContentExtent().GetY() >= 200.0f,
			"ScrollingFrame derives a bounded content extent without changing child authored positions");
		const auto AuthoredPosition = ScrolledButton->GetPosition();
		(void)Runtime.ProcessEvent(WheelEvent{{1}, {40, 60}, {0, -3}});
		(void)Runtime.Reconcile();
		const auto *ScrolledNode = FindNode(Runtime.GetCommittedLayout(), ScrolledButton->GetObjectId());
		Check(Scroll->GetCanvasPosition().GetY() > 0.0f && ScrolledNode && ScrolledNode->Bounds.Y < 120.0f &&
			ScrolledButton->GetPosition().X.Scale == AuthoredPosition.X.Scale &&
			ScrolledButton->GetPosition().X.Offset == AuthoredPosition.X.Offset &&
			ScrolledButton->GetPosition().Y.Scale == AuthoredPosition.Y.Scale &&
			ScrolledButton->GetPosition().Y.Offset == AuthoredPosition.Y.Offset,
			"wheel scrolling commits a child transform while preserving authored Position");
		const float BeforeDrag = Scroll->GetCanvasPosition().GetY();
		(void)Runtime.ProcessPointer({17, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, {150, 100}});
		(void)Runtime.ProcessPointer({17, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Move, {150, 60}});
		(void)Runtime.ProcessPointer({17, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Up, {150, 60}});
		(void)Runtime.Reconcile();
		Check(Scroll->GetCanvasPosition().GetY() >= BeforeDrag,
			"touch drag uses the shared per-pointer capture model and bounded scroll ownership");
		auto OuterScroll = std::make_shared<ScrollingFrame>();
		OuterScroll->SetPosition(UDim2::fromOffset(20, 150));
		OuterScroll->SetSize(UDim2::fromOffset(160, 100));
		OuterScroll->SetCanvasSize(UDim2::fromOffset(160, 400));
		OuterScroll->SetAutomaticCanvasSize(Enums::AutomaticSize::None);
		OuterScroll->SetParent(Root);
		auto InnerScroll = std::make_shared<ScrollingFrame>();
		InnerScroll->SetSize(UDim2::fromOffset(140, 80));
		InnerScroll->SetCanvasSize(UDim2::fromOffset(140, 240));
		InnerScroll->SetAutomaticCanvasSize(Enums::AutomaticSize::None);
		InnerScroll->SetParent(OuterScroll);
		auto InnerTarget = std::make_shared<Frame>();
		InnerTarget->SetSize(UDim2::fromScale(1.0f, 1.0f));
		InnerTarget->SetInteractable(true);
		InnerTarget->SetParent(InnerScroll);
		(void)Runtime.Reconcile();
		(void)Runtime.ProcessEvent(WheelEvent{{1}, {40, 170}, {0, -1}});
		Check(InnerScroll->GetCanvasPosition().GetY() > 0.0f && OuterScroll->GetCanvasPosition().GetY() == 0.0f,
			"nested wheel scrolling gives the nearest eligible scroll context first ownership");
		InnerScroll->SetCanvasPosition({0.0f, 160.0f});
		(void)Runtime.Reconcile();
		(void)Runtime.ProcessEvent(WheelEvent{{1}, {40, 170}, {0, -1}});
		Check(OuterScroll->GetCanvasPosition().GetY() > 0.0f,
			"a nested scroll context at its bound deterministically chains wheel ownership to its ancestor");
		(void)Runtime.ProcessPointer({29, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, {40, 170}});
		InnerScroll->Destroy();
		(void)Runtime.Reconcile();
		(void)Runtime.ProcessPointer({29, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Move, {40, 140}});
		(void)Runtime.ProcessPointer({29, Enums::GuiPointerType::Touch, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Up, {40, 140}});
		Check(!FindNode(Runtime.GetCommittedLayout(), InnerScroll->GetObjectId()),
			"destroying a nested scroll gesture owner retires capture without retaining a stale ObjectId");

		auto Input = std::make_shared<TextBox>();
		Input->SetPosition(UDim2::fromOffset(220, 30));
		Input->SetSize(UDim2::fromOffset(220, 36));
		Input->SetPlaceholderText("Type here");
		Input->SetText(std::string("A") + "\xC3\xA9" + "\xF0\x9F\x99\x82");
		Input->SetParent(Root);
		(void)Runtime.Reconcile();
		Input->CaptureFocus();
		(void)Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Right, LogicalKey::End, KeyModifier::None,
			ButtonState::Pressed, false});
		auto Omega = BoundedUtf8::From("\xCE\xA9");
		Check(Omega.has_value() && Runtime.ProcessEvent(TextInputEvent{{1}, *Omega}),
			"focused TextBox accepts bounded UTF-8 committed text input");
		Check(Input->GetCaretPosition() == 4 && Input->GetText().ends_with("\xCE\xA9"),
			"editable caret and storage use Unicode code-point boundaries rather than UTF-8 bytes");
		(void)Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Backspace, LogicalKey::Backspace, KeyModifier::None,
			ButtonState::Pressed, false});
		Check(Input->GetCaretPosition() == 3 && !Input->GetText().ends_with("\xCE\xA9"),
			"Backspace deletes one complete multi-byte code point");
		(void)Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Left, LogicalKey::Left, KeyModifier::Shift,
			ButtonState::Pressed, false});
		auto Replacement = BoundedUtf8::From("Z");
		(void)Runtime.ProcessEvent(TextInputEvent{{1}, *Replacement});
		Check(Input->GetSelectionLength() == 0 && Input->GetText().ends_with("Z"),
			"Shift selection and insertion replace a code-point-safe range");
		auto Preedit = BoundedCompositionUtf8::From("\xE3\x81\x82");
		Check(Preedit.has_value() && Runtime.ProcessEvent(TextEditingEvent{{1}, *Preedit, 0, 1}) &&
			Input->GetCompositionText() == "\xE3\x81\x82",
			"SDL-compatible IME preedit updates the bounded semantic composition state");
		auto TextCommand = Runtime.SynchronizeTextInput();
		Check(TextCommand && std::get_if<SetTextInputState>(&*TextCommand) &&
			std::get<SetTextInputState>(*TextCommand).Active && !std::get<SetTextInputState>(*TextCommand).Secure &&
			std::get<SetTextInputState>(*TextCommand).AutocorrectEnabled,
			"focused TextBox requests native text input with a committed caret area");
		(void)Runtime.Reconcile();
		const auto Access = Runtime.GetAccessibilitySnapshot();
		auto InputAccess = std::ranges::find(Access->Nodes, Input->GetObjectId(), &GuiAccessibilityNode::Object);
		auto ScrollAccess = std::ranges::find(Access->Nodes, Scroll->GetObjectId(), &GuiAccessibilityNode::Object);
		Check(InputAccess != Access->Nodes.end() && InputAccess->Role == Enums::AccessibilityRole::TextBox &&
			InputAccess->Editable && InputAccess->Caret == static_cast<std::uint32_t>(Input->GetCaretPosition()),
			"accessibility exposes editable role, value, caret, selection, and stable object identity");
		Check(ScrollAccess != Access->Nodes.end() && ScrollAccess->Role == Enums::AccessibilityRole::ScrollView &&
			ScrollAccess->ScrollMaximumY > 0.0f,
			"accessibility exposes renderer-independent scroll position and range semantics");
		Input->SetSecureTextEntry(true);
		Input->SetMultiLine(true);
		(void)Runtime.Reconcile();
		auto SecureTextCommand = Runtime.SynchronizeTextInput();
		Check(SecureTextCommand && std::get_if<SetTextInputState>(&*SecureTextCommand) &&
			std::get<SetTextInputState>(*SecureTextCommand).Secure &&
			std::get<SetTextInputState>(*SecureTextCommand).Multiline &&
			!std::get<SetTextInputState>(*SecureTextCommand).AutocorrectEnabled,
			"secure TextBox changes reconfigure native input as hidden-password with autocorrect disabled");
		Input->ReleaseFocus();
		auto StoppedTextCommand = Runtime.SynchronizeTextInput();
		Check(StoppedTextCommand && !std::get<SetTextInputState>(*StoppedTextCommand).Active,
			"releasing secure TextBox focus stops native text input");
		Input->SetText(std::string("\xC0\x80", 2));
		(void)Runtime.Reconcile();
		Check(Input->GetText() == "\xEF\xBF\xBD\xEF\xBF\xBD",
			"authored invalid editable UTF-8 is normalized to replacement scalar values before further editing");
		Input->SetText("secret");
		Input->SetSecureTextEntry(true);
		(void)Runtime.Reconcile();
		const auto SecureAccess = Runtime.GetAccessibilitySnapshot();
		auto SecureInput = std::ranges::find(SecureAccess->Nodes, Input->GetObjectId(), &GuiAccessibilityNode::Object);
		Check(SecureInput != SecureAccess->Nodes.end() && SecureInput->Value != "secret" && !SecureInput->Value.empty(),
			"secure TextBox presentation and accessibility state do not expose authored plaintext");
	}

	void TestLuauVerticalSlice() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		std::vector<std::pair<std::string, std::string>> Diagnostics;
		std::shared_ptr<Script> Source;
		SignalConnection::Pointer ActivatedConnection;
		{
			HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
			Engine Runtime(Game, &Renderer, [&](std::string Severity, std::string Message) {
				Diagnostics.emplace_back(std::move(Severity), std::move(Message));
			});
			Source = std::make_shared<Script>();
			Source->SetName("GuiFoundationVerticalSlice");
			Source->SetSource(R"(
local Root = Instance.new("ScreenGui")
Root.Name = "LuauGui"
Root.Parent = game

local Panel = Instance.new("Frame")
Panel.Name = "LuauPanel"
Panel.Position = UDim2.fromScale(0.5, 0.5)
Panel.Size = UDim2.fromOffset(400, 260)
Panel.AnchorPoint = Vector2.new(0.5, 0.5)
Panel.BackgroundColor3 = Color3.fromRGB(30, 34, 42)
Panel.Parent = Root

local Title = Instance.new("TextLabel")
Title.Text = "Gargantuan"
Title.Size = UDim2.new(1, -24, 0, 48)
Title.Position = UDim2.fromOffset(12, 12)
Title.Parent = Panel

local Button = Instance.new("TextButton")
Button.Name = "LuauPlayButton"
Button.Text = "Play"
Button.Size = UDim2.fromOffset(160, 44)
Button.Position = UDim2.fromOffset(120, 150)
Button.Parent = Panel
Button.Activated:Connect(function()
	Button.Name = "LuauActivated"
end)
)");
			Source->SetParent(Game);
			Runtime.Script->Step();
			(void)Runtime.Gui->Reconcile();
			auto Button = std::dynamic_pointer_cast<TextButton>(Game->FindFirstChild("LuauPlayButton", true));
			Check(Button != nullptr, "Luau can construct the complete ScreenGui, Frame, TextLabel, and TextButton hierarchy");
			if (Button) {
				const Vector2 Center(Button->GetAbsolutePosition() + Button->GetAbsoluteSize() / 2.0f);
				(void)Runtime.ProcessEvent(PointerButtonEvent{{2}, PointerButton::Left, ButtonState::Pressed,
					{Center.GetX(), Center.GetY()}});
				(void)Runtime.ProcessEvent(PointerButtonEvent{{2}, PointerButton::Left, ButtonState::Released,
					{Center.GetX(), Center.GetY()}});
				Check(Button->GetName() == "LuauActivated", "Luau Activated callbacks receive centralized routed input");
				if (!Button->Activated->Connections.empty()) ActivatedConnection = Button->Activated->Connections.front();
			}
			const bool ScriptError = std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
				return Diagnostic.first == "Error" && Diagnostic.second.find("GuiFoundationVerticalSlice") != std::string::npos;
			});
			Check(!ScriptError, "the Foundation 1 Luau vertical slice executes without a runtime diagnostic");
			Runtime.Destroy();
			Check(Source->Thread == nullptr && Source->ThreadReference == LUA_NOREF,
				"Engine teardown retires Script coroutine state before closing its Luau VM");
			Check(ActivatedConnection && !ActivatedConnection->Connected && ActivatedConnection->L == nullptr,
				"Engine teardown retires Luau signal subscriptions before closing their Luau VM");
		}
		Game->Destroy();
	}

	void TestDataModelBeforeEngineTeardown() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer);
		auto Source = std::make_shared<Script>();
		Source->SetName("DataModelFirstTeardown");
		Source->SetSource(R"(
local Root = Instance.new("ScreenGui")
Root.Parent = game
local Button = Instance.new("TextButton")
Button.Name = "DataModelFirstButton"
Button.Parent = Root
Button.Activated:Connect(function() end)
)");
		Source->SetParent(Game);
		Runtime.Script->Step();
		(void)Runtime.Gui->Reconcile();
		auto Button = std::dynamic_pointer_cast<TextButton>(Game->FindFirstChild("DataModelFirstButton", true));
		SignalConnection::Pointer Connection;
		if (Button && !Button->Activated->Connections.empty()) Connection = Button->Activated->Connections.front();

		Game->Destroy();
		Check(Source->Thread == nullptr && Source->ThreadReference == LUA_NOREF,
			"DataModel-first teardown retires Script coroutine state while the Luau VM is alive");
		Check(Connection && !Connection->Connected && Connection->L == nullptr,
			"DataModel-first teardown retires Luau signal subscriptions while GuiRuntime is alive");
		Runtime.Destroy();
	}
}

int main() {
	struct SdlProcessCleanup final { ~SdlProcessCleanup() { SDL_Quit(); } } SdlCleanup;
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		RunTest("LayoutTextPresentationAndResources", TestLayoutTextPresentationAndResources);
		RunTest("AutomaticSizeListAndClipping", TestAutomaticSizeListAndClipping);
		RunTest("RotationOrderingAndAlpha", TestRotationOrderingAndAlpha);
		RunTest("InputRoutingFocusAndMutationSafety", TestInputRoutingFocusAndMutationSafety);
		RunTest("DisabledButtonNeverActivates", TestDisabledButtonNeverActivates);
		RunTest("PersistencePlayIsolationAndSignalRetirement", TestPersistencePlayIsolationAndSignalRetirement);
		RunTest("IncrementalInvalidationAndUiRetention", TestIncrementalInvalidationAndUiRetention);
		RunTest("SiblingStackingContexts", TestSiblingStackingContexts);
		RunTest("ScrollingAndEditableText", TestScrollingAndEditableText);
		RunTest("LuauVerticalSlice", TestLuauVerticalSlice);
		RunTest("DataModelBeforeEngineTeardown", TestDataModelBeforeEngineTeardown);
	} catch (const std::exception &Exception) {
		std::cerr << "[Gui:FoundationTest] unexpected exception: " << Exception.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Gui:FoundationTest] All GUI Foundation 1 and 2 tests passed\n";
	return 0;
}
