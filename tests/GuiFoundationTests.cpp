// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/Engine.hpp"
#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/UIListLayout.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

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
		Runtime.RegisterImage("test/checker", 2, 2, Pixels);
		Pixels[0] = 240;
		Runtime.RegisterImage("test/checker", 2, 2, Pixels);
		auto Image = std::make_shared<ImageLabel>();
		Image->SetImage("test/checker");
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
		Check(Publication->FullResync && !Publication->TextureCreates.empty() && !Publication->Ui.Batches.empty(),
			"GUI resources and display data use the existing RenderPublication UI and texture contracts");
		RenderProjection Projection;
		auto Applied = Projection.Apply(*Publication);
		Check(Applied.TexturesCreated == Publication->TextureCreates.size() && Applied.UiBatches == Publication->Ui.Batches.size(),
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
			Restart->Ui.Batches.size() == Publication->Ui.Batches.size(),
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

	void TestPersistencePlayIsolationAndSignalRetirement() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto Tree = BuildTree(Game);
		Game->MarkPersistenceSubtreeArchivable();
		std::shared_ptr<Instance> RootValue = Game;
		const auto Serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, RootValue);
		Check(Serialized.find("ScreenGui") != std::string::npos && Serialized.find("TextButton") != std::string::npos &&
			Serialized.find("Gargantuan") != std::string::npos && Serialized.find("AccessibleName") != std::string::npos,
			"ordinary schema persistence saves GUI hierarchy, text, and accessibility semantics");
		Check(Serialized.find("AbsolutePosition") == std::string::npos && Serialized.find("GuiState") == std::string::npos,
			"persistence excludes committed layout and transient interaction state");
		std::istringstream Input(Serialized);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		Check(Loaded.Ok && Loaded.Instance && Loaded.Instance->FindFirstChild("PlayButton", true),
			"GUI semantic state and hierarchy reconstruct through the ordinary project format");

		PlaySession Session({7301}, Serialized, InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(), 800, 600, Game->GetAuthoritativeRevision());
		auto RuntimeWorld = Session.GetWorld();
		auto RuntimeButton = std::dynamic_pointer_cast<TextButton>(RuntimeWorld->FindFirstChild("PlayButton", true));
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
		Session.Stop();
		Check(Tree.Button->GetText() == "Play" && !Tree.Button->GetDestroyed() &&
			!Game->FindFirstChild("RuntimeOnlyGui", true),
			"runtime GUI focus, mutations, creation, and destruction do not contaminate the authoring DataModel");

		int Calls = 0;
		auto Connection = Tree.Button->Activated->Connect([&](std::monostate) { ++Calls; });
		Tree.Button->Destroy();
		Tree.Button->Activated->Fire({});
		Check(!Connection->Connected && Calls == 0, "destroying a GUI object retires its existing Gargantuan Signal connections");
	}

	void TestLuauVerticalSlice() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		std::vector<std::pair<std::string, std::string>> Diagnostics;
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer, [&](std::string Severity, std::string Message) {
			Diagnostics.emplace_back(std::move(Severity), std::move(Message));
		});
		auto Source = std::make_shared<Script>();
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
		}
		const bool ScriptError = std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
			return Diagnostic.first == "Error" && Diagnostic.second.find("GuiFoundationVerticalSlice") != std::string::npos;
		});
		Check(!ScriptError, "the Foundation 1 Luau vertical slice executes without a runtime diagnostic");
		Runtime.Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestLayoutTextPresentationAndResources();
		TestAutomaticSizeListAndClipping();
		TestRotationOrderingAndAlpha();
		TestInputRoutingFocusAndMutationSafety();
		TestPersistencePlayIsolationAndSignalRetirement();
		TestLuauVerticalSlice();
	} catch (const std::exception &Exception) {
		std::cerr << "[Gui:FoundationTest] unexpected exception: " << Exception.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Gui:FoundationTest] All GUI Foundation 1 tests passed\n";
	return 0;
}
