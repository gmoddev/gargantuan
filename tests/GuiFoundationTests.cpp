// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/Engine.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/assets/GasDependencies.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageButton.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/ScrollingFrame.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/TextBox.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/UIListLayout.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/services/TweenService.hpp"
#include "BenchmarkAllocationProbe.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
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

	void TestApplicationMenuProjection() {
		using namespace gargantuan;
		auto App = std::make_shared<DataModel>();
		auto First = BuildTree(App);
		auto Second = BuildTree(App);
		auto OtherWorld = std::make_shared<DataModel>();
		auto Foreign = BuildTree(OtherWorld);
		GuiRuntime Runtime(App, std::filesystem::path(DefaultGuiFontPath), GuiRuntimeMode::Application);
		Runtime.SetViewport({800, 600, 1.0f, {}});
		(void)Runtime.Reconcile();
		Check(Runtime.GetCommittedLayout()->Nodes.empty(), "application starts with no projected menu");
		Check(Runtime.SetApplicationMenu(First.Root, true), "register a live application ScreenGui");
		(void)Runtime.Reconcile();
		const auto FirstFrame = Runtime.GetApplicationFrame();
		Check(FindNode(Runtime.GetCommittedLayout(), First.Button->GetObjectId()) &&
			!FindNode(Runtime.GetCommittedLayout(), Second.Button->GetObjectId()), "only registered menu is projected");
		Check(!Runtime.SetApplicationMenu(Foreign.Root, true), "foreign menu leaves current selection intact");
		auto ForeignFrame = FirstFrame;
		ForeignFrame.Source = OtherWorld->GetObjectId();
		Check(!Runtime.AcknowledgeApplicationFrame(ForeignFrame),
			"foreign surface receipt cannot enable application input");
		int Activations = 0;
		First.Button->Activated->Connect([&](auto) { ++Activations; });
		const GuiPointerInput Down{0, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary,
			GuiPointerAction::Down, {400, 340}};
		auto Up = Down;
		Up.Action = GuiPointerAction::Up;
		Check(!Runtime.ProcessPointer(Down), "unpresented application menu rejects press");
		Check(Runtime.AcknowledgeApplicationFrame(FirstFrame), "acknowledge displayed menu");
		(void)Runtime.ProcessPointer(Down, FirstFrame);
		(void)Runtime.ProcessPointer(Up, FirstFrame);
		Check(Activations == 1, "displayed application button accepts a click without a Player");
		(void)Runtime.ProcessPointer(Down, FirstFrame);
		Check(Runtime.SetApplicationMenu(Second.Root, true), "replace menu with a live successor");
		Check(!Runtime.ProcessPointer(Up, FirstFrame) && Activations == 1, "replacing menu cancels pending press");
		(void)Runtime.Reconcile();
		Check(!Runtime.AcknowledgeApplicationFrame(FirstFrame), "stale presentation rejected");
		const auto SecondFrame = Runtime.GetApplicationFrame();
		Check(Runtime.AcknowledgeApplicationFrame(SecondFrame), "acknowledge replacement");
		Check(!Runtime.ProcessPointer(Down, FirstFrame) && !Runtime.ProcessPointer(Up, FirstFrame),
			"queued click on prior menu cannot activate its replacement");
		Runtime.SetViewport({800, 600, 2.0f, {}});
		Check(!Runtime.ProcessPointer(Down, SecondFrame), "DPI change invalidates old hit coordinates immediately");
		(void)Runtime.Reconcile();
		const auto ResizedFrame = Runtime.GetApplicationFrame();
		Runtime.InvalidateApplicationSurface();
		Check(!Runtime.AcknowledgeApplicationFrame(ResizedFrame), "old device receipt rejected after restart");
		Check(Runtime.SetApplicationMenu(Second.Root, false), "hide the selected application menu");
		(void)Runtime.Reconcile();
		Check(Runtime.GetCommittedLayout()->Nodes.empty(), "hidden root has no layout or hits");
		Check(Runtime.SetApplicationMenu(Second.Root, true), "show same menu instance");
		Second.Root->Destroy();
		(void)Runtime.Reconcile();
		Check(Runtime.GetCommittedLayout()->Nodes.empty(), "destroying selection does not select a fallback root");
	}

	void TestApplicationBindings() {
		using namespace gargantuan;
		auto Graph = std::make_shared<DataModel>();
		auto FolderValue = std::make_shared<Folder>();
		FolderValue->SetParent(Graph);
		auto Entry = std::make_shared<Script>();
		Entry->SetParent(FolderValue);
		Entry->SetRunContext(Enums::RunContext::Client);
		Entry->SetSource(R"(
			local Menu = Instance.new("ScreenGui")
			Menu.Parent = script.Parent
			assert(game:SetMainMenu(Menu).Ok and game:IsMainMenuVisible())
			assert(game:SetMainMenu(script).Code == "InvalidMenu")
			local Frame = 0
			game:GetService("RunService").PreRender:Connect(function()
				Frame += 1
				if Frame == 1 then
					assert(game:SetMainMenuVisible(false).Ok)
				elseif Frame == 2 then
					assert(game:SetMainMenuVisible(true).Ok)
				end
			end)
		)");
		ApplicationPresentation App(Graph, Entry, std::filesystem::path(DefaultGuiFontPath));
		Check(App.IsMainMenuVisible(), "registered title is visible on successful entry");
		App.Step(0.016f);
		Check(!App.IsMainMenuVisible(), "managed presentation callback hides the menu");
		App.Step(0.016f);
		Check(App.IsMainMenuVisible(), "application callbacks advance before there is a gameplay session");
		Check(!Graph->FindFirstChildWhichIsA("Player", true), "application VM does not allocate a Player");
		auto Foreign = std::make_shared<DataModel>();
		auto VM = std::make_shared<ScriptEngine>(Foreign);
		auto Ordinary = std::make_shared<Script>();
		Ordinary->SetParent(Foreign);
		Ordinary->SetRunContext(Enums::RunContext::Client);
		Ordinary->SetSource("assert(game:SetMainMenu(nil).Code == 'NotAuthorized')");
		VM->RunBootstrapScript(Ordinary);
		Check(Ordinary->Status == ScriptStatus::Finished, "ordinary client cannot register an application menu");
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

	void TestAutomaticSizePolicyTransition() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		Game->InitializeLoadedProjectRevision();
		const auto Starter = Game->FindFirstChildOfClass("StarterGui", false);
		auto Screen = std::make_shared<ScreenGui>();
		Screen->SetParent(Starter);
		auto Panel = std::make_shared<Frame>();
		Panel->SetSize(UDim2::fromOffset(150, 70));
		Panel->SetParent(Screen);
		auto Child = std::make_shared<Frame>();
		Child->SetSize(UDim2::fromOffset(32, 20));
		Child->SetPosition(UDim2::fromOffset(5, 4));
		Child->SetParent(Panel);
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath), GuiRuntimeMode::AuthoringPreview);
		Runtime.SetViewport({400, 300, 1.0f, {}});
		const auto CheckLayout = [&](float Width, bool Automatic, const char *Message) {
			(void)Runtime.Reconcile();
			const auto Snapshot = Runtime.GetPresentedLayout();
			const auto *Node = Snapshot ? FindNode(Snapshot, Panel->GetObjectId()) : nullptr;
			Check(Node && Near(Node->Bounds.Width, Width) && Near(Node->Bounds.Height, 70.0f) &&
				Node->LayoutSizesX == Automatic && !Node->LayoutSizesY, Message);
		};
		CheckLayout(150.0f, false, "independent layout keeps the authored size before AutomaticSize changes");
		Panel->SetAutomaticSize(Enums::AutomaticSize::X);
		CheckLayout(37.0f, true, "enabling AutomaticSize on an independent root rebuilds the presented size and ownership");
		Child->SetPosition(UDim2::fromOffset(13, 4));
		CheckLayout(45.0f, true, "child placement changes remeasure the now-dependent root");
		Panel->SetAutomaticSize(Enums::AutomaticSize::None);
		CheckLayout(150.0f, false, "disabling AutomaticSize retires layout ownership and restores the authored size");
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

	void TestStarterGuiPreviewAndRuntimeRoots() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		Game->InitializeLoadedProjectRevision();
		const auto Starter = Game->FindFirstChildOfClass("StarterGui", false);
		auto Template = BuildTree(Game);
		Template.Root->SetParent(Starter);
		Template.Root->MarkPersistenceSubtreeArchivable();
		const auto Legacy = BuildTree(Game);
		const auto OriginalPosition = Template.Button->GetAbsolutePosition();
		const auto OriginalSize = Template.Button->GetAbsoluteSize();
		const auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
		const std::array<std::uint8_t, 4> Pixel{56, 92, 32, 255};
		auto Image = std::make_shared<ImageLabel>();
		Image->SetImage(Assets->RegisterMemoryImage("PreviewFixture", 1, 1, Pixel));
		Image->SetSize(UDim2::fromOffset(32, 32));
		Image->SetParent(Template.Root);
		Image->SetArchivable(true);
		const auto ImageRevision = Game->GetAuthoritativeRevision();
		const auto PositionSignal = Template.Button->GetPropertyChangedSignal("Position");
		const auto OriginalConnections = PositionSignal->Connections.size();
		{
			GuiRuntime Preview(Game, std::filesystem::path(DefaultGuiFontPath), GuiRuntimeMode::AuthoringPreview);
			Preview.SetViewport({800, 600, 1.0f, {}});
			(void)Preview.Reconcile();
			const auto Layout = Preview.GetCommittedLayout();
			const auto *Button = FindNode(Layout, Template.Button->GetObjectId());
			const auto *ImageNode = FindNode(Layout, Image->GetObjectId());
			Check(Button && Button->Bounds.Width == 160.0f &&
				!FindNode(Layout, Legacy.Button->GetObjectId()),
				"authoring preview resolves direct StarterGui templates and excludes legacy runtime roots");
			Check(ImageNode && ImageNode->Presentation.ImageTexture.has_value() &&
				ImageNode->Presentation.ImageTexture->IsValid() &&
				!Assets->DrainTextureChanges().Creates.empty(),
				"preview resolves the canonical image without consuming the editor upload queue");
			int Activations = 0;
			auto Activation = Template.Button->Activated->Connect([&](std::monostate) { ++Activations; });
			Preview.RequestFocus(Template.Button->GetObjectId());
			if (Button) {
				const Vector2 Center(Button->Bounds.X + 4.0f, Button->Bounds.Y + 4.0f);
				for (const auto Action : {GuiPointerAction::Down, GuiPointerAction::Up})
					Check(!Preview.ProcessPointer({1, Enums::GuiPointerType::Mouse,
						Enums::GuiPointerButton::Primary, Action, Center}), "preview leaves pointer input to the editor");
			}
			Check(Activations == 0 && !GuiRuntime::Find(*Template.Button) && !Preview.SynchronizeTextInput() &&
				Template.Button->GetAbsolutePosition() == OriginalPosition && Template.Button->GetAbsoluteSize() == OriginalSize &&
				Game->GetAuthoritativeRevision() == ImageRevision,
				"preview never registers for runtime input or mutates authored geometry and revision");
			Activation->Disconnect();
			Template.Button->SetParent(nullptr);
			Template.Button->SetParent(Template.Root);
			Check(PositionSignal->Connections.size() == OriginalConnections + 1,
				"moving a preview control removes its old layout observation before it is registered again");
		}
		Check(PositionSignal->Connections.size() == OriginalConnections,
			"preview retirement disconnects observers on authored objects that remain open");
		Template.Button->SetPosition(UDim2::fromOffset(24, 30));
		auto PlayersService = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		auto First = PlayersService->CreateSessionPlayer({"test", "gui-first"});
		auto Second = PlayersService->CreateSessionPlayer({"test", "gui-second"});
		Check(PlayersService->SetTrustedLocalPlayer(First), "runtime GUI fixture installs a trusted local Player");
		auto FirstGui = First->GetPlayerGui().value();
		auto SecondGui = Second->GetPlayerGui().value();
		auto FirstButton = FirstGui->FindFirstChild("PlayButton", true);
		auto SecondButton = SecondGui->FindFirstChild("PlayButton", true);
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({800, 600, 1.0f, {}});
		(void)Runtime.Reconcile();
		const auto Layout = Runtime.GetCommittedLayout();
		Check(FirstButton && SecondButton && FindNode(Layout, FirstButton->GetObjectId()) &&
			!FindNode(Layout, SecondButton->GetObjectId()) && !FindNode(Layout, Template.Button->GetObjectId()) &&
			FindNode(Layout, Legacy.Button->GetObjectId()),
			"runtime projects the matching PlayerGui and legacy roots while templates and other owners stay out");
		const auto AddFocusTarget = [&] {
			const auto Target = std::make_shared<TextBox>();
			Target->SetSize(UDim2::fromOffset(160, 32));
			Target->SetParent(FirstGui->FindFirstChildOfClass("ScreenGui", false));
			return Target;
		};
		const auto FirstFocus = AddFocusTarget();
		const auto RequestedFocus = AddFocusTarget();
		const auto RedirectedFocus = AddFocusTarget();
		(void)Runtime.Reconcile();
		FirstFocus->CaptureFocus();
		std::vector<std::string_view> FocusEvents;
		const auto Lost = FirstFocus->FocusLost->Connect([&](std::monostate) {
			FocusEvents.emplace_back("FirstLost");
			RedirectedFocus->CaptureFocus();
		});
		const auto UnexpectedGain = RequestedFocus->Focused->Connect([&](std::monostate) { FocusEvents.emplace_back("RequestedGained"); });
		const auto UnexpectedLoss = RequestedFocus->FocusLost->Connect([&](std::monostate) { FocusEvents.emplace_back("RequestedLost"); });
		const auto RedirectedGain = RedirectedFocus->Focused->Connect([&](std::monostate) { FocusEvents.emplace_back("RedirectedGained"); });
		const auto RedirectedLoss = RedirectedFocus->FocusLost->Connect([&](std::monostate) {
			FocusEvents.emplace_back("RedirectedLost");
			RequestedFocus->CaptureFocus();
		});
		RequestedFocus->CaptureFocus();
		Runtime.ClearTransientState();
		Check(FocusEvents == std::vector<std::string_view>{"FirstLost", "RedirectedGained", "RedirectedLost"},
			"an owner GUI can redirect focus without a stale notification and clear focus without reacquiring it");
		for (const auto &Observer : {Lost, UnexpectedGain, UnexpectedLoss, RedirectedGain, RedirectedLoss}) Observer->Disconnect();
		Game->Destroy();
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

	void TestImageButtonPresentationAndActivation() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		GuiRuntime Runtime(Game, std::filesystem::path(DefaultGuiFontPath));
		Runtime.SetViewport({800, 600, 2.0f, {}});
		auto Root = std::make_shared<ScreenGui>();
		Root->SetParent(Game);
		auto Panel = std::make_shared<Frame>();
		Panel->SetPosition(UDim2::fromOffset(20, 30));
		Panel->SetSize(UDim2::fromOffset(100, 75));
		Panel->SetClipsDescendants(true);
		Panel->SetParent(Root);
		auto Button = std::make_shared<ImageButton>();
		Button->SetSize(UDim2::fromOffset(100, 100));
		Button->SetAccessibleName("Open map");
		const std::vector<std::uint8_t> Pixels(4 * 2 * 4, 255);
		const auto Reference = Runtime.RegisterImage("test/image-button", 4, 2, Pixels);
		Button->SetImage(Reference);
		Button->SetParent(Panel);
		Check(Button->GetInteractable() && Button->GetSelectable() &&
			Button->GetInputSink() == Enums::InputSink::Activate &&
			Button->GetScaleType() == Enums::GuiImageScaleType::Stretch,
			"ImageButton inherits Stretch and defaults to an enabled focusable control");

		const auto CheckQuad = [&](const RenderUiFrame &FrameValue, float X, float Y, float Right, float Bottom) {
			const auto Batch = std::ranges::find_if(FrameValue.Batches, [](const RenderUiBatch &Value) {
				return Value.Texture.has_value();
			});
			return Batch != FrameValue.Batches.end() && Batch->Vertices.size() == 4 && Batch->Clip &&
				Near(Batch->Clip->X, 40) && Near(Batch->Clip->Y, 60) &&
				Near(Batch->Clip->Width, 200) && Near(Batch->Clip->Height, 150) &&
				Near(Batch->Vertices[0].Position.x, X) && Near(Batch->Vertices[0].Position.y, Y) &&
				Near(Batch->Vertices[2].Position.x, Right) && Near(Batch->Vertices[2].Position.y, Bottom) &&
				Batch->Vertices[0].TextureCoordinate == glm::vec2(0, 0) &&
				Batch->Vertices[2].TextureCoordinate == glm::vec2(1, 1);
		};
		(void)Runtime.Reconcile();
		const auto Stretch = Runtime.GetCommittedPresentation();
		Check(CheckQuad(Stretch->Frame, 40, 60, 240, 260),
			"Stretch fills the unchanged control rectangle and retains its ancestor clip");
		Button->SetScaleType(Enums::GuiImageScaleType::Fit);
		(void)Runtime.Reconcile();
		Check(CheckQuad(Runtime.GetCommittedPresentation()->Frame, 40, 110, 240, 210) &&
			CheckQuad(Stretch->Frame, 40, 60, 240, 260),
			"changing ScaleType centers a wide image while retaining the earlier immutable publication");
		Check(Runtime.RegisterImage("test/image-button", 2, 4, Pixels) == Reference,
			"an asset dimension change retains the logical image reference");
		(void)Runtime.Reconcile();
		Check(CheckQuad(Runtime.GetCommittedPresentation()->Frame, 90, 60, 190, 260),
			"the same image reference fits again when its dimensions change");

		auto Pointer = [&](GuiPointerAction Action, Vector2 Point = {50.0f, 70.0f}) {
			return Runtime.ProcessPointer({7, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::Primary, Action, Point});
		};
		auto Key = [&](LogicalKey Logical, ButtonState State) {
			return Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Space, Logical, KeyModifier::None, State, false});
		};
		std::vector<std::string> Events;
		Button->Focused->Connect([&](std::monostate) { Events.emplace_back("Focus"); });
		Button->PointerDown->Connect([&](std::shared_ptr<GuiInputEvent>) { Events.emplace_back("Down"); });
		Button->PointerUp->Connect([&](std::shared_ptr<GuiInputEvent>) { Events.emplace_back("Up"); });
		Button->Activated->Connect([&](std::monostate) { Events.emplace_back("Activate"); });
		const bool Down = Pointer(GuiPointerAction::Down);
		const bool Up = Pointer(GuiPointerAction::Up);
		Check(Down && Up && Events == std::vector<std::string>{"Focus", "Down", "Up", "Activate"},
			"the Fit margin remains clickable and activates after normal focus and pointer routing");
		(void)Runtime.Reconcile();
		const auto Access = Runtime.GetAccessibilitySnapshot();
		const auto Index = Access->NodeByObject.find(Button->GetObjectId());
		Check(Index != Access->NodeByObject.end() &&
			Access->Nodes[Index->second].Role == Enums::AccessibilityRole::Button &&
			Access->Nodes[Index->second].Name == "Open map" && Access->Nodes[Index->second].Focused,
			"ImageButton exposes a named, focusable Button to the accessibility projection");

		Events.clear();
		(void)Key(LogicalKey::Space, ButtonState::Pressed);
		(void)Key(LogicalKey::Space, ButtonState::Released);
		(void)Key(LogicalKey::Return, ButtonState::Pressed);
		(void)Key(LogicalKey::Return, ButtonState::Released);
		Check(Events == std::vector<std::string>{"Activate", "Activate"},
			"a focused ImageButton uses the same Space and Return activation as TextButton");
		Events.clear();
		(void)Pointer(GuiPointerAction::Down, {50.0f, 230.0f});
		(void)Pointer(GuiPointerAction::Up, {50.0f, 230.0f});
		Check(Events.empty(), "an ImageButton cannot activate outside its ancestor clip");
		(void)Pointer(GuiPointerAction::Down);
		Button->SetInteractable(false);
		(void)Pointer(GuiPointerAction::Up);
		(void)Key(LogicalKey::Space, ButtonState::Pressed);
		(void)Key(LogicalKey::Space, ButtonState::Released);
		Check(Events == std::vector<std::string>{"Down", "Up"} && !Button->GetActive(),
			"disabling a captured ImageButton prevents pointer and keyboard activation");
		Button->SetInteractable(true);
		(void)Runtime.Reconcile();
		Events.clear();
		(void)Pointer(GuiPointerAction::Down);
		Button->Destroy();
		(void)Pointer(GuiPointerAction::Up);
		(void)Runtime.Reconcile();
		Check(Events == std::vector<std::string>{"Down"} &&
			!Runtime.GetAccessibilitySnapshot()->NodeByObject.contains(Button->GetObjectId()),
			"destroying a captured ImageButton retires activation and accessibility state");
		Game->Destroy();
	}

	void TestImageControlGasPersistence() {
		using namespace gargantuan;
		constexpr std::array<std::uint8_t, 58> Bmp{
			'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
			40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
			0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0,
		};
		struct TempDirectory {
			std::filesystem::path Root;
			~TempDirectory() {
				std::error_code Ignored;
				std::filesystem::remove(Root / "Icon.bmp", Ignored);
				std::filesystem::remove(Root, Ignored);
			}
		} Directory{std::filesystem::temp_directory_path() / ("gargantuan-image-button-" + AssetId::New().ToString())};
		std::filesystem::create_directory(Directory.Root);
		{
			std::ofstream File(Directory.Root / "Icon.bmp", std::ios::binary);
			File.write(reinterpret_cast<const char *>(Bmp.data()), static_cast<std::streamsize>(Bmp.size()));
			if (!File) throw std::runtime_error("Could not write ImageButton asset fixture");
		}
		DiskFilesystem Filesystem(Directory.Root);
		SourceMount Mount(Filesystem);
		auto Assets = std::make_shared<AssetService>();
		const auto LabelImage = Assets->ImportProjectAsset(Mount, "Icon.bmp", AssetKind::Image, "Label image");
		const auto ButtonImage = Assets->ImportProjectAsset(Mount, "Icon.bmp", AssetKind::Image, "Button image");
		Check(LabelImage.Ok && LabelImage.Record && ButtonImage.Ok && ButtonImage.Record,
			"the GAS fixture has separately identified project image assets");
		if (!LabelImage.Ok || !LabelImage.Record || !ButtonImage.Ok || !ButtonImage.Record) return;
		auto Panel = std::make_shared<Frame>();
		auto Label = std::make_shared<ImageLabel>();
		Label->SetName("Icon");
		Label->SetImage(LabelImage.Record->Reference.Value);
		Label->SetParent(Panel);
		auto Button = std::make_shared<ImageButton>();
		Button->SetName("OpenMap");
		Button->SetImage(ButtonImage.Record->Reference.Value);
		Button->SetScaleType(Enums::GuiImageScaleType::Fit);
		Button->SetAccessibleName("Open map");
		Button->SetParent(Panel);
		Panel->MarkPersistenceSubtreeArchivable();
		const std::array<std::shared_ptr<Instance>, 1> Roots{Panel};
		auto Document = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Model);
		Check(Document && Document->Nodes.size() == 3, "GASM captures the panel and both authored image controls");
		if (!Document || Document->Nodes.size() != 3) return;
		auto Pins = Gas::ResolveDependencies(*Document, Assets.get(), false);
		Check(Pins && Pins->size() == 2 &&
			std::ranges::find(*Pins, Gas::Dependency{LabelImage.Record->Id, LabelImage.Record->ContentId}) != Pins->end() &&
			std::ranges::find(*Pins, Gas::Dependency{ButtonImage.Record->Id, ButtonImage.Record->ContentId}) != Pins->end(),
			"GAS discovers asset identity and digest from ImageLabel and the inherited ImageButton property");
		if (!Pins) return;
		Document->Dependencies = std::move(*Pins);
		const auto Encoded = Gas::Encode(*Document);
		Check(Encoded.has_value(), "GASM encodes image layout and dependency pins");
		if (!Encoded) return;
		const auto Decoded = Gas::Decode(*Encoded);
		Check(Decoded && Decoded->Dependencies == Document->Dependencies, "GASM preserves both image pins");
		if (!Decoded) return;
		const auto Restored = InstanceSerialization::MaterializeGasDocument(*Decoded, false);
		Check(Restored.Succeeded(), "GASM materializes image controls");
		if (!Restored.Succeeded()) return;
		const auto RestoredLabel = std::dynamic_pointer_cast<ImageLabel>(Restored.Roots.front()->FindFirstChild("Icon", true));
		const auto RestoredButton = std::dynamic_pointer_cast<ImageButton>(Restored.Roots.front()->FindFirstChild("OpenMap", true));
		Check(RestoredLabel && RestoredLabel->GetImage() == LabelImage.Record->Reference.Value &&
			RestoredLabel->GetScaleType() == Enums::GuiImageScaleType::Stretch &&
			RestoredButton && RestoredButton->GetImage() == ButtonImage.Record->Reference.Value &&
			RestoredButton->GetScaleType() == Enums::GuiImageScaleType::Fit && RestoredButton->GetSelectable() &&
			RestoredButton->GetAccessibleName() == "Open map" && !RestoredButton->GetActive(),
			"GASM round trips authored image controls while initialization starts with inactive runtime state");
		Restored.Roots.front()->Destroy();
		Panel->Destroy();
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

local Icon = Instance.new("ImageButton")
Icon.Name = "LuauIconButton"
Icon.Size = UDim2.fromOffset(32, 32)
Icon.ScaleType = Enum.GuiImageScaleType.Fit
Icon.AccessibleName = "Open map"
Icon.Parent = Panel
)");
			Source->SetParent(Game);
			Runtime.Script->Step();
			(void)Runtime.Gui->Reconcile();
			auto Button = std::dynamic_pointer_cast<TextButton>(Game->FindFirstChild("LuauPlayButton", true));
			Check(Button != nullptr, "Luau can construct the complete ScreenGui, Frame, TextLabel, and TextButton hierarchy");
			auto Icon = std::dynamic_pointer_cast<ImageButton>(Game->FindFirstChild("LuauIconButton", true));
			Check(Icon && Icon->GetScaleType() == Enums::GuiImageScaleType::Fit && Icon->GetAccessibleName() == "Open map",
				"Luau constructs ImageButton and authors its inherited image and accessibility properties");
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

	void TestGuiEventDestroysEngine() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer);
		auto Tree = BuildTree(Game);
		auto Source = std::make_shared<Script>();
		Source->SetName("DestroyGameFromGuiEvent");
		Source->SetSource(R"(
			local Button = game:FindFirstChild('PlayButton', true)
			Button.Activated:Connect(function() game:Destroy() end)
		)");
		Source->SetParent(Game);
		Runtime.Script->Step();
		(void)Runtime.Gui->Reconcile();
		const Vector2 Center(Tree.Button->GetAbsolutePosition() + Tree.Button->GetAbsoluteSize() / 2.0f);
		(void)Runtime.ProcessEvent(PointerButtonEvent{{2}, PointerButton::Left, ButtonState::Pressed,
			{Center.GetX(), Center.GetY()}});
		(void)Runtime.ProcessEvent(PointerButtonEvent{{2}, PointerButton::Left, ButtonState::Released,
			{Center.GetX(), Center.GetY()}});
		Check(Game->GetDestroyed() && Tree.Button->GetDestroyed() && !Runtime.Gui && !Runtime.Script,
			"GUI callback destroys Engine without releasing active event routing state");
		const auto AfterDestroy = Runtime.ProcessEvent(FocusEvent{true});
		Check(!AfterDestroy.Command && !AfterDestroy.Consumed,
			"destroyed Engine ignores later host input without reading retired GUI state");
	}

	void TestFocusClearDestroysEngine() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer);
		auto Tree = BuildTree(Game);
		auto Input = std::make_shared<TextBox>();
		Input->SetName("FocusExitBox");
		Input->SetSize(UDim2::fromOffset(180, 32));
		Input->SetPosition(UDim2::fromOffset(12, 60));
		Input->SetParent(Tree.Root);
		(void)Runtime.Gui->Reconcile();
		Input->CaptureFocus();
		int FocusLostCalls = 0;
		auto Observer = Input->FocusLost->Connect([&](std::monostate) { ++FocusLostCalls; });
		auto Source = std::make_shared<Script>();
		Source->SetName("DestroyGameOnFocusLost");
		Source->SetSource(R"(
			local Input = game:FindFirstChild('FocusExitBox', true)
			Input.FocusLost:Connect(function() game:Destroy() end)
		)");
		Source->SetParent(Game);
		Runtime.Script->Step();
		Check(Source->Status == ScriptStatus::Finished && !Game->GetDestroyed(),
			"focused GUI installs a FocusLost destruction callback");
		(void)Runtime.ProcessEvent(FocusEvent{false});
		Check(Game->GetDestroyed() && Input->GetDestroyed() && FocusLostCalls == 1 &&
			!Runtime.Gui && !Runtime.Script,
			"nested GUI teardown sends FocusLost exactly once and clears focus safely");
		Observer->Disconnect();
	}

	std::shared_ptr<gargantuan::Script> RunTweenScript(gargantuan::Engine &Runtime, const char *Source,
		gargantuan::Enums::RunContext Context = gargantuan::Enums::RunContext::Server) {
		using namespace gargantuan;
		auto ScriptValue = std::make_shared<Script>();
		ScriptValue->SetName("TweenExercise");
		ScriptValue->SetRunContext(Context);
		ScriptValue->SetSource(Source);
		ScriptValue->SetParent(Runtime.DataModel);
		Runtime.Script->RunBootstrapScript(ScriptValue);
		if (ScriptValue->Status != ScriptStatus::Finished)
			throw std::runtime_error("[Tween:Test] " + ScriptValue->ErrorMessage);
		return ScriptValue;
	}

	void TestTweenInterpolationAndLifecycle() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer);
		auto Tree = BuildTree(Game);
		Tree.Panel->SetBackgroundColor3(Color3(0, 0, 0));
		Tree.Panel->SetPosition(UDim2::fromOffset(0, 0));
		auto Block = std::make_shared<Part>();
		Block->SetName("TweenBlock");
		Block->SetAnchored(true);
		Block->SetParent(Runtime.Workspace);
		auto Source = RunTweenScript(Runtime, R"(
local Service = game:GetService("TweenService")
local Panel = game.FoundationGui.Panel
local Block = game.Workspace.TweenBlock
local Info = TweenInfo.new(2, Enum.EasingStyle.Linear, Enum.EasingDirection.In, 1, true, 1)
local OversizedGoals = table.create(1024)
local AcceptedGoals, GoalError = pcall(function() Service:Create(Panel, Info, OversizedGoals) end)
assert(not AcceptedGoals and string.find(tostring(GoalError), "256 backing slots", 1, true),
	"an oversized empty goal table must be rejected before iteration")
assert(TweenInfo.new().Time == 1 and Info.DelayTime == 1)
assert(not pcall(TweenInfo.new, 1, Enum.EasingDirection.In))
assert(not pcall(TweenInfo.new, math.huge))
assert(not pcall(TweenInfo.new, 1, nil, nil, 0.5))
assert(not pcall(TweenInfo.new, 1, nil, nil, -1))
assert(not pcall(function() Service:Create(Panel, Info, {}) end))
assert(not pcall(function() Service:Create(Panel, Info, {Name = "changed"}) end))
assert(not pcall(function() Service:Create(Panel, Info, {AbsoluteSize = Vector2.new(1, 1)}) end))
assert(not pcall(function() Service:Create(Panel, Info, {ZIndex = "1e100"}) end))
assert(not pcall(function() Service:Create(Panel, Info, {ZIndex = 1.5}) end))
assert(not pcall(function() Service:Create(Panel, Info, {Rotation = math.huge}) end))
assert(not pcall(function() Service:Create(Panel, Info, setmetatable({Rotation = 1}, {})) end))
assert(not pcall(function() Service:Create(Instance.new("Frame"), Info, {Rotation = 1}) end))

local Fade = Service:Create(Panel, Info, {
	BackgroundTransparency = 1,
	BackgroundColor3 = Color3.new(0.2, 0.4, 0.8),
	Position = UDim2.fromOffset(30, 48),
	ZIndex = 6,
})
assert(typeof(Fade) == "Tween" and Fade.PlaybackState == Enum.PlaybackState.Begin)
assert(Fade.TweenInfo.Time == 2)
Fade.Completed:Once(function(State)
	assert(State == Enum.PlaybackState.Completed)
	Panel:SetAttribute("FadeCompleted", true)
	Fade:Play()
end)
Fade:Play()
assert(Fade.PlaybackState == Enum.PlaybackState.Delayed)

local Move = Service:Create(Block, TweenInfo.new(2, Enum.EasingStyle.Linear), {
	CFrame = CFrame.new(10, 8, 6) * CFrame.Angles(0, math.pi / 2, 0),
	Size = Vector3.new(3, 5, 7),
})
Move:Play()
)");
		Runtime.Script->StepTweens(1.0f);
		const auto Half = Block->GetCFrame();
		Check(Near(Half.Position.x, 5) && Near(Half.Position.y, 4) && Near(Half.Position.z, 3) &&
			Near(Half.GetLookVector().x, -std::sqrt(0.5f)) && Near(Block->GetSize().z, 4),
			"anchored Part advances reflected CFrame and Vector3 halfway");
		Check(Near(Tree.Panel->GetBackgroundTransparency(), 0) && Tree.Panel->GetPosition().Y.Offset == 0,
			"delayed GUI tween holds its verified start at the delay boundary");
		Runtime.Script->StepTweens(1.0f);
		const auto Color = Tree.Panel->GetBackgroundColor3();
		Check(Near(Tree.Panel->GetBackgroundTransparency(), 0.5f) && Near(Color.R, 0.1f) &&
			Near(Color.G, 0.2f) && Near(Color.B, 0.4f) &&
			Tree.Panel->GetPosition().Y.Offset == 24 && Tree.Panel->GetZIndex() == 3,
			"GUI tween interpolates scalar, distinct color channels, offset layout and integer goals");
		Runtime.Script->StepTweens(7.0f);
		Check(Tree.Panel->GetAttributeValue("FadeCompleted") == WireValue(true) &&
			Near(Tree.Panel->GetBackgroundTransparency(), 0) && Tree.Panel->GetPosition().Y.Offset == 0,
			"finite repeat and reverse finish at the start and reentrant Play waits for another step");
		Runtime.Script->StepTweens(2.0f);
		Check(Near(Tree.Panel->GetBackgroundTransparency(), 0.5f), "replay recaptures its initial values");
		Source->Destroy();
		Runtime.Script->StepTweens(10.0f);
		Check(Near(Tree.Panel->GetBackgroundTransparency(), 0.5f), "destroying the tween's Script retires playback");

		for (const auto &[Style, Directions] : EasingCurves::CURVES) {
			for (const auto &[Direction, Curve] : Directions) {
				Check(EasingCurves::CalculateAlpha(0, Style, Direction) == 0 &&
					EasingCurves::CalculateAlpha(1, Style, Direction) == 1 &&
					std::isfinite(EasingCurves::CalculateAlpha(0.25f, Style, Direction)),
					"every easing curve has exact boundaries and a finite interior");
			}
		}
	}

	void TestTweenConflictAndAuthority() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer);
		const auto Tree = BuildTree(Game);
		auto Locked = std::make_shared<Frame>();
		Locked->SetName("Locked");
		Locked->SetParent(Game->GetService("ReplicatedStorage"));
		auto ReplacementTarget = std::make_shared<Frame>();
		ReplacementTarget->SetName("RetiredPanel");
		ReplacementTarget->SetParent(Tree.Root);
		const auto RetiredId = ReplacementTarget->GetObjectId();
		auto LocalPlayer = Runtime.Players->GetLocalPlayer().value_or(nullptr);
		auto OtherPlayer = Runtime.Players->CreateSessionPlayer({"test", "tween-observer"});
		OtherPlayer->SetName("TweenObserver");
		if (!LocalPlayer) throw std::runtime_error("[Tween:Test] Offline local player was not established");
		const auto BuildPlayerGui = [](const std::shared_ptr<Player> &Owner) {
			auto Root = std::make_shared<ScreenGui>();
			Root->SetParent(Owner->GetPlayerGui().value());
			auto Panel = std::make_shared<Frame>();
			Panel->SetName("OwnerPanel");
			Panel->SetParent(Root);
			return Panel;
		};
		auto LocalPanel = BuildPlayerGui(LocalPlayer);
		auto OtherPanel = BuildPlayerGui(OtherPlayer);
		(void)RunTweenScript(Runtime, R"(
local Service = game.TweenService
local Button = game.FoundationGui.Panel.PlayButton
Service:Create(Button, TweenInfo.new(1, Enum.EasingStyle.Linear), {Rotation = 74}):Play()
)");
		(void)RunTweenScript(Runtime, R"(
local Service = game.TweenService
local Panel = game.FoundationGui.Panel
local Info = TweenInfo.new(1, Enum.EasingStyle.Linear)
assert(not pcall(function() Service:Create(game.ReplicatedStorage.Locked, Info, {Rotation = 20}) end))
assert(not pcall(function() Service:Create(Panel.PlayButton, Info, {Rotation = 10}):Play() end))
local LocalPanel = game.Players.LocalPlayer.PlayerGui:FindFirstChild("OwnerPanel", true)
local OtherContainer = game.Players.TweenObserver:FindFirstChildOfClass("PlayerGui", false)
local OtherPanel = OtherContainer and OtherContainer:FindFirstChild("OwnerPanel", true)
assert(OtherPanel and not pcall(function() Service:Create(OtherPanel, Info, {Rotation = 10}) end))
local Immediate = Service:Create(LocalPanel, TweenInfo.new(0), {Rotation = 20})
Immediate:Play()
local First = Service:Create(Panel, Info, {Rotation = 40, ZIndex = 6})
local Second = Service:Create(Panel, Info, {Rotation = 100, ZIndex = 8})
First.Completed:Once(function(State)
	assert(State == Enum.PlaybackState.Cancelled)
	Panel:SetAttribute("Conflict", true)
end)
Second.Completed:Once(function(State)
	assert(State == Enum.PlaybackState.Cancelled and Panel:GetAttribute("Conflict"))
	Panel:SetAttribute("Reentry", true)
end)
First:Play()
First:Pause()
assert(First.PlaybackState == Enum.PlaybackState.Paused)
Second:Play()
Second:Pause()
Second:Resume()
assert(Second.PlaybackState == Enum.PlaybackState.Playing and First.PlaybackState == Enum.PlaybackState.Cancelled)
Panel:GetPropertyChangedSignal("Rotation"):Once(function() Second:Cancel() end)
local Title = Panel.Title
local PauseDuringWrite = Service:Create(Title, Info, {Rotation = 80, ZIndex = 8})
Title:GetPropertyChangedSignal("Rotation"):Once(function()
	PauseDuringWrite:Pause()
	PauseDuringWrite:Resume()
end)
PauseDuringWrite:Play()
local Retiring = Service:Create(game.FoundationGui.RetiredPanel, Info, {Rotation = 100})
Retiring.Completed:Once(function(State)
	assert(State == Enum.PlaybackState.Cancelled)
	Panel:SetAttribute("TargetRetired", true)
end)
Retiring:Play()
)", Enums::RunContext::Client);
		ReplacementTarget->Destroy();
		auto Replacement = std::make_shared<Frame>();
		Replacement->SetParent(Tree.Root);
		const auto ReplacementId = Replacement->GetObjectId();
		Runtime.Script->StepTweens(0.5f);
		Check(Near(Tree.Panel->GetRotation(), 50) && Tree.Panel->GetZIndex() == 0 &&
			Tree.Panel->GetAttributeValue("Reentry") == WireValue(true),
			"an overlap cancels the prior tween, and cancellation inside one property setter prevents later writes");
		Check(ReplacementId.Slot == RetiredId.Slot && ReplacementId.Generation != RetiredId.Generation &&
			Near(Replacement->GetRotation(), 0) && Tree.Panel->GetAttributeValue("TargetRetired") == WireValue(true),
			"destroyed target cancels and fresh generation skips stale tween work");
		Check(Near(Locked->GetRotation(), 0), "client tween cannot change a server-authoritative service property");
		Check(Near(LocalPanel->GetRotation(), 20) && Near(OtherPanel->GetRotation(), 0),
			"zero-duration client tween updates the local PlayerGui only");
		Check(Near(Tree.Button->GetRotation(), 37), "client cannot replace an authoritative tween in a shared offline VM");
		Check(Near(Tree.Title->GetRotation(), 40) && Tree.Title->GetZIndex() == 0,
			"reentrant pause and resume pause the remaining writes until a later step");
		Runtime.Script->StepTweens(0.5f);
		Check(Near(Tree.Title->GetRotation(), 80) && Tree.Title->GetZIndex() == 8,
			"the resumed playback finishes on the next sample");
	}

	void TestTweenPopulationAndShutdown() {
		using namespace gargantuan;
		using namespace gargantuan::runtime_detail;
		auto Game = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(800.0f, 600.0f));
		Engine Runtime(Game, &Renderer);
		(void)RunTweenScript(Runtime, R"(
local Pool = Instance.new("Folder")
Pool.Name = "TweenPool"
Pool.Parent = game
local Service = game.TweenService
local Info = TweenInfo.new(1, Enum.EasingStyle.Linear)
for Index = 1, 1024 do
	local Panel = Instance.new("Frame")
	Panel.Parent = Pool
	local Tween = Service:Create(Panel, Info, {Rotation = 100})
	Tween:Play()
	if Index == 1 then
		Tween.Completed:Connect(function() game:SetAttribute("UnexpectedCompletion", true) end)
	end
end
local Excess = Instance.new("Frame")
Excess.Parent = game
assert(not pcall(function() Service:Create(Excess, Info, {Rotation = 100}):Play() end))
)");
		WorkSample Sample{};
		BenchmarkAllocationProbe::Counts Allocations;
		{
			BenchmarkAllocationProbe::Scope Probe(Allocations);
			WorkCapture Capture(&Sample, BenchmarkAllocationProbe::ReadCalls, BenchmarkAllocationProbe::ReadBytes);
			for (int Step = 0; Step < 10; ++Step) Runtime.Script->StepTweens(0.05f);
		}
		const auto &Work = Sample[static_cast<std::size_t>(WorkPhase::TweenStep)];
		Check(Work.Calls == 10 && Work.Units == TweenRuntime::MaximumPlaybacks * Work.Calls && Work.Rejected == 0,
			"permanent work diagnostics count every tween across bounded scheduler passes");
		std::cout << "[Tween:Qualification] active=" << TweenRuntime::MaximumPlaybacks
			<< " steps=" << Work.Calls << " samples=" << Work.Units << " elapsed_ns=" << Work.Nanoseconds
			<< " cpp_allocations=" << Work.Allocations << " cpp_allocated_bytes=" << Work.AllocatedBytes << '\n';
		const auto Pool = Game->FindFirstChild("TweenPool", false);
		Check(Pool && Pool->Children.size() == TweenRuntime::MaximumPlaybacks &&
			Near(static_cast<Frame &>(*Pool->Children.front()).GetRotation(), 50) &&
			Near(static_cast<Frame &>(*Pool->Children.back()).GetRotation(), 50),
			"the active playback bound advances each retained tween equally");
		Runtime.Script->ShutdownRuntime();
		Runtime.Script->StepTweens(0.5f);
		Check(!Game->GetAttributeValue("UnexpectedCompletion"), "shutdown discards pending playback notifications");
	}
}

int main() {
	struct SdlProcessCleanup final { ~SdlProcessCleanup() { SDL_Quit(); } } SdlCleanup;
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		RunTest("LayoutTextPresentationAndResources", TestLayoutTextPresentationAndResources);
		RunTest("ApplicationMenuProjection", TestApplicationMenuProjection);
		RunTest("ApplicationBindings", TestApplicationBindings);
		RunTest("AutomaticSizeListAndClipping", TestAutomaticSizeListAndClipping);
		RunTest("AutomaticSizePolicyTransition", TestAutomaticSizePolicyTransition);
		RunTest("RotationOrderingAndAlpha", TestRotationOrderingAndAlpha);
		RunTest("StarterGuiPreviewAndRuntimeRoots", TestStarterGuiPreviewAndRuntimeRoots);
		RunTest("InputRoutingFocusAndMutationSafety", TestInputRoutingFocusAndMutationSafety);
		RunTest("DisabledButtonNeverActivates", TestDisabledButtonNeverActivates);
		RunTest("ImageButtonPresentationAndActivation", TestImageButtonPresentationAndActivation);
		RunTest("ImageControlGasPersistence", TestImageControlGasPersistence);
		RunTest("PersistencePlayIsolationAndSignalRetirement", TestPersistencePlayIsolationAndSignalRetirement);
		RunTest("IncrementalInvalidationAndUiRetention", TestIncrementalInvalidationAndUiRetention);
		RunTest("SiblingStackingContexts", TestSiblingStackingContexts);
		RunTest("ScrollingAndEditableText", TestScrollingAndEditableText);
		RunTest("LuauVerticalSlice", TestLuauVerticalSlice);
		RunTest("DataModelBeforeEngineTeardown", TestDataModelBeforeEngineTeardown);
		RunTest("GuiEventDestroysEngine", TestGuiEventDestroysEngine);
		RunTest("FocusClearDestroysEngine", TestFocusClearDestroysEngine);
		RunTest("TweenInterpolationAndLifecycle", TestTweenInterpolationAndLifecycle);
		RunTest("TweenConflictAndAuthority", TestTweenConflictAndAuthority);
		RunTest("TweenPopulationAndShutdown", TestTweenPopulationAndShutdown);
	} catch (const std::exception &Exception) {
		std::cerr << "[Gui:FoundationTest] unexpected exception: " << Exception.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Gui:FoundationTest] All GUI Foundation 1 and 2 tests passed\n";
	return 0;
}
