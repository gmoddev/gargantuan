// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/gui/GuiRuntime.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/classes/LayerCollector.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/ScrollingFrame.hpp"
#include "gargantuan/classes/TextBox.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/UIListLayout.hpp"
#include "gargantuan/gui/GuiLimits.hpp"
#include "gargantuan/render/RenderExtractor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <ranges>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gargantuan {
	namespace {
		using Clock = std::chrono::steady_clock;
		constexpr std::size_t NoParent = std::numeric_limits<std::size_t>::max();

		enum DirtyDomain : std::uint32_t {
			DirtyNone = 0,
			DirtyLayout = 1u << 0,
			DirtyPresentation = 1u << 1,
			DirtyText = 1u << 2,
			DirtyInput = 1u << 3,
			DirtyAccessibility = 1u << 4,
			DirtyResource = 1u << 5,
			DirtyHierarchy = 1u << 6,
			DirtyScroll = 1u << 7,
			DirtyAll = DirtyLayout | DirtyPresentation | DirtyText | DirtyInput | DirtyAccessibility |
				DirtyResource | DirtyHierarchy | DirtyScroll,
		};

		std::uint64_t Nanoseconds(Clock::duration Duration) {
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count());
		}

		float ClampUnit(float Value) {
			return std::clamp(std::isfinite(Value) ? Value : 0.0f, 0.0f, 1.0f);
		}

		float Resolve(const UDim &Value, float Parent) {
			const float Result = Parent * Value.Scale + static_cast<float>(Value.Offset);
			return std::isfinite(Result) ? Result : 0.0f;
		}

		GuiRect TransformBounds(const GuiTransform &Transform, const GuiRect &Bounds) {
			const std::array<Vector2, 4> Corners{
				Transform.Apply({Bounds.X, Bounds.Y}),
				Transform.Apply({Bounds.X + Bounds.Width, Bounds.Y}),
				Transform.Apply({Bounds.X + Bounds.Width, Bounds.Y + Bounds.Height}),
				Transform.Apply({Bounds.X, Bounds.Y + Bounds.Height}),
			};
			float MinimumX = Corners[0].GetX();
			float MaximumX = MinimumX;
			float MinimumY = Corners[0].GetY();
			float MaximumY = MinimumY;
			for (const auto &Corner : Corners) {
				MinimumX = std::min(MinimumX, Corner.GetX());
				MaximumX = std::max(MaximumX, Corner.GetX());
				MinimumY = std::min(MinimumY, Corner.GetY());
				MaximumY = std::max(MaximumY, Corner.GetY());
			}
			return {MinimumX, MinimumY, MaximumX - MinimumX, MaximumY - MinimumY};
		}

		bool SameClip(const std::optional<RenderUiClipRect> &Left, const std::optional<RenderUiClipRect> &Right) {
			if (Left.has_value() != Right.has_value()) return false;
			if (!Left) return true;
			return Left->X == Right->X && Left->Y == Right->Y && Left->Width == Right->Width &&
				Left->Height == Right->Height;
		}

		bool SameVisualPresentation(const GuiResolvedPresentation &Left, const GuiResolvedPresentation &Right) {
			return Left.Kind == Right.Kind && Left.BackgroundColor.R == Right.BackgroundColor.R &&
				Left.BackgroundColor.G == Right.BackgroundColor.G && Left.BackgroundColor.B == Right.BackgroundColor.B &&
				Left.BackgroundAlpha == Right.BackgroundAlpha && Left.ContentColor.R == Right.ContentColor.R &&
				Left.ContentColor.G == Right.ContentColor.G && Left.ContentColor.B == Right.ContentColor.B &&
				Left.ContentAlpha == Right.ContentAlpha && Left.ImageTexture == Right.ImageTexture &&
				Left.Text == Right.Text && Left.TextOffsetX == Right.TextOffsetX && Left.TextOffsetY == Right.TextOffsetY &&
				Left.DrawCaret == Right.DrawCaret && Left.CaretX == Right.CaretX && Left.SelectionX == Right.SelectionX &&
				Left.SelectionWidth == Right.SelectionWidth;
		}

		Enums::GuiPointerButton ConvertButton(PointerButton Button) {
			switch (Button) {
			case PointerButton::Left: return Enums::GuiPointerButton::Primary;
			case PointerButton::Right: return Enums::GuiPointerButton::Secondary;
			case PointerButton::Middle: return Enums::GuiPointerButton::Middle;
			default: return Enums::GuiPointerButton::None;
			}
		}

		struct NormalizedEditableText {
			std::string Bytes;
			std::vector<std::size_t> Boundaries{0};
			bool Replaced = false;
			bool Truncated = false;
		};

		NormalizedEditableText NormalizeEditableUtf8(std::string_view Text) {
			NormalizedEditableText Result;
			Result.Bytes.reserve(std::min(Text.size(), GuiLimits::MaximumEditableTextBytes));
			Result.Boundaries.reserve(std::min(Text.size() + 1, GuiLimits::MaximumEditableCodePoints + 1));
			std::size_t Index = 0;
			while (Index < Text.size() && Result.Boundaries.size() <= GuiLimits::MaximumEditableCodePoints &&
				Result.Bytes.size() < GuiLimits::MaximumEditableTextBytes) {
				const auto First = static_cast<unsigned char>(Text[Index]);
				std::size_t Length = 0;
				std::uint32_t Codepoint = 0;
				if (First < 0x80) { Length = 1; Codepoint = First; }
				else if ((First & 0xe0) == 0xc0) { Length = 2; Codepoint = First & 0x1f; }
				else if ((First & 0xf0) == 0xe0) { Length = 3; Codepoint = First & 0x0f; }
				else if ((First & 0xf8) == 0xf0) { Length = 4; Codepoint = First & 0x07; }
				bool Valid = Length != 0 && Index + Length <= Text.size();
				for (std::size_t Offset = 1; Valid && Offset < Length; ++Offset) {
					const auto Byte = static_cast<unsigned char>(Text[Index + Offset]);
					Valid = (Byte & 0xc0) == 0x80;
					Codepoint = (Codepoint << 6) | (Byte & 0x3f);
				}
				if (Valid) {
					const bool Overlong = (Length == 2 && Codepoint < 0x80) || (Length == 3 && Codepoint < 0x800) ||
						(Length == 4 && Codepoint < 0x10000);
					Valid = !Overlong && Codepoint <= 0x10ffff && !(Codepoint >= 0xd800 && Codepoint <= 0xdfff);
				}
				if (!Valid) {
					if (Result.Bytes.size() + 3 > GuiLimits::MaximumEditableTextBytes) break;
					Result.Bytes.append("\xEF\xBF\xBD", 3);
					++Index;
					Result.Replaced = true;
				} else {
					if (Result.Bytes.size() + Length > GuiLimits::MaximumEditableTextBytes) break;
					Result.Bytes.append(Text.substr(Index, Length));
					Index += Length;
				}
				Result.Boundaries.push_back(Result.Bytes.size());
			}
			Result.Truncated = Index < Text.size();
			return Result;
		}

		std::vector<std::size_t> Utf8Boundaries(std::string_view Text) {
			return NormalizeEditableUtf8(Text).Boundaries;
		}

		std::string MaskEditableText(std::string_view Text) {
			const auto Boundaries = NormalizeEditableUtf8(Text).Boundaries;
			std::string Result;
			Result.reserve((Boundaries.size() - 1) * 3);
			for (std::size_t Index = 1; Index < Boundaries.size(); ++Index) Result.append("\xE2\x80\xA2", 3);
			return Result;
		}

		std::mutex RuntimeRegistryMutex;
		std::unordered_map<DataModel *, GuiRuntime *> RuntimeRegistry;
	}

	struct GuiRuntime::Impl {
		struct WorkingNode {
			std::shared_ptr<GuiObject> Object;
			std::size_t Parent = NoParent;
			std::uint32_t Depth = 0;
			std::uint32_t OriginalOrder = 0;
			GuiRect Bounds;
			GuiTransform Transform;
			GuiRect TransformedBounds;
			std::optional<GuiRect> Clip;
			std::shared_ptr<const GuiShapedText> Text;
			std::optional<AssetImageResource> Image;
			float AutoWidth = std::numeric_limits<float>::quiet_NaN();
			float AutoHeight = std::numeric_limits<float>::quiet_NaN();
		};

		struct ListConfiguration {
			Enums::GuiFillDirection Direction = Enums::GuiFillDirection::Vertical;
			UDim Padding;
			std::vector<std::size_t> Children;
		};

		struct RootCache {
			ObjectId Root;
			int DisplayOrder = 0;
			Enums::ZIndexBehavior ZIndexBehavior = Enums::ZIndexBehavior::Global;
			std::vector<GuiLayoutNode> Nodes;
			std::unordered_map<ObjectId, std::size_t> NodeByObject;
			bool AutomaticFallback = false;
			bool HasDependentLayout = false;
		};

		struct PresentationSpan {
			std::size_t Batch = 0;
			std::uint32_t FirstVertex = 0;
		};

		struct TextEditingState {
			std::size_t Caret = 0;
			std::size_t Anchor = 0;
			std::string Composition;
			std::size_t CompositionSelectionStart = 0;
			std::size_t CompositionSelectionLength = 0;
			float HorizontalOffset = 0.0f;
		};

		struct ScrollGesture {
			ObjectId Scroll;
			ObjectId InitialTarget;
			Vector2 StartPhysical;
			Vector2 LastPhysical;
			bool Dragging = false;
		};

		std::shared_ptr<DataModel> World;
		std::shared_ptr<AssetService> Assets;
		GuiViewportConfiguration Viewport;
		GuiTextSystem Text;
		std::function<void()> DescendantBinding;
		SignalConnection::Pointer DescendantRemovedConnection;
		std::unordered_map<ObjectId, std::vector<SignalConnection::Pointer>> Observers;
		std::unordered_map<ObjectId, RootCache> RootCaches;
		std::vector<std::shared_ptr<ScreenGui>> Roots;
		std::unordered_map<ObjectId, std::uint32_t> RootDirty;
		std::unordered_map<ObjectId, std::uint32_t> ObjectDirty;
		std::unordered_map<ObjectId, std::uint64_t> DirtyEpochs;
		std::unordered_map<ObjectId, PresentationSpan> SolidPresentationSpans;
		std::unordered_set<ObjectId> PendingVisualObjects;
		std::uint64_t NextDirtyEpoch = 1;
		std::unordered_map<int, ObjectId> Hovered;
		std::unordered_map<int, ObjectId> Pressed;
		std::unordered_map<int, ObjectId> Captured;
		std::unordered_map<ObjectId, ObjectId> FocusedByRoot;
		std::unordered_map<ObjectId, TextEditingState> TextEditing;
		std::unordered_map<int, ScrollGesture> ScrollGestures;
		ObjectId KeyboardPressed;
		bool TextInputCommandDirty = true;
		bool TextInputWasActive = false;
		std::shared_ptr<const GuiLayoutSnapshot> Layout = std::make_shared<const GuiLayoutSnapshot>();
		std::shared_ptr<const GuiPresentationSnapshot> Presentation = std::make_shared<const GuiPresentationSnapshot>();
		std::shared_ptr<const GuiAccessibilitySnapshot> Accessibility = std::make_shared<const GuiAccessibilitySnapshot>();
		GuiTextureChanges PendingTextures;
		GuiRuntimeProfile Profile;
		std::uint64_t PendingObservationNanoseconds = 0;
		std::uint64_t PendingDirtyMarkingNanoseconds = 0;
		std::deque<GuiDiagnostic> Diagnostics;
		std::uint64_t NextDiagnosticSequence = 1;
		std::uint64_t NextLayoutGeneration = 1;
		std::uint64_t NextPresentationGeneration = 1;
		std::uint64_t NextAccessibilityGeneration = 1;
		std::uint64_t AssetChangeSequence = 1;
		std::size_t PresentationVertices = 0;
		std::size_t PresentationIndices = 0;
		std::size_t LayoutPassesThisFrame = 0;
		std::size_t ShapedGlyphsThisFrame = 0;
		std::size_t TextEditsThisFrame = 0;
		std::size_t SelectionOperationsThisFrame = 0;
		std::unordered_set<ObjectId> MeasuredTextThisFrame;
		bool StructureDirty = true;
		bool PresentationDirty = true;

		static std::shared_ptr<AssetService> PrepareAssets(
			const std::shared_ptr<DataModel> &World,
			const std::filesystem::path &FontPath
		) {
			if (!World) throw std::invalid_argument("GuiRuntime requires a DataModel");
			auto Service = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
			if (!Service) throw std::runtime_error("AssetService schema resolved to an incompatible service");
			Service->ConfigureBuiltInFont(FontPath);
			return Service;
		}

		Impl(std::shared_ptr<DataModel> WorldValue, std::filesystem::path FontPath)
			: World(std::move(WorldValue)), Assets(PrepareAssets(World, FontPath)), Text(Assets) {
			if (!World) throw std::invalid_argument("GuiRuntime requires a DataModel");
			Viewport = {1, 1, 1.0f, {}};
			DescendantBinding = World->BindDescendants([this](std::shared_ptr<Instance> Object) {
				const auto Start = Clock::now();
				Observe(Object);
				if (IsGuiRelevant(Object)) StructureDirty = true;
				PendingObservationNanoseconds += Nanoseconds(Clock::now() - Start);
			});
			DescendantRemovedConnection = World->DescendantRemoved->Connect([this](std::shared_ptr<Instance> Object) {
				const auto Id = Object->GetObjectId();
				Observers.erase(Id);
				ObjectDirty.erase(Id);
				DirtyEpochs.erase(Id);
				TextEditing.erase(Id);
				if (IsGuiRelevant(Object)) StructureDirty = true;
			});
			if (!Text.HasFont()) AddDiagnostic("MissingFont", "The controlled GargantuanSans font fixture is unavailable", {});
		}

		~Impl() {
			if (DescendantBinding) DescendantBinding();
			if (DescendantRemovedConnection) DescendantRemovedConnection->Disconnect();
			Observers.clear();
		}

		bool IsGuiRelevant(const std::shared_ptr<Instance> &Object) const {
			if (!Object) return false;
			if (std::dynamic_pointer_cast<GuiBase>(Object)) return true;
			return Object->FindFirstAncestorWhichIsA("ScreenGui") != nullptr;
		}

		void AddDiagnostic(std::string Code, std::string Message, ObjectId Object) {
			if (Message.size() > GuiLimits::MaximumDiagnosticBytes) Message.resize(GuiLimits::MaximumDiagnosticBytes);
			const bool Duplicate = std::ranges::any_of(Diagnostics, [&](const GuiDiagnostic &Existing) {
				return Existing.Code == Code && Existing.Object == Object;
			});
			if (Duplicate) return;
			if (Diagnostics.size() == GuiLimits::MaximumDiagnostics) Diagnostics.pop_front();
			Diagnostics.push_back({NextDiagnosticSequence++, std::move(Code), std::move(Message), Object});
			LOG_WARN(App, "[Gui:Runtime] %s", Diagnostics.back().Message.c_str());
		}

		std::shared_ptr<ScreenGui> FindRoot(const std::shared_ptr<Instance> &Object) const {
			for (auto Current = Object; Current; Current = Current->GetParent().value_or(nullptr))
				if (auto Root = std::dynamic_pointer_cast<ScreenGui>(Current)) return Root;
			return nullptr;
		}

		void Mark(ObjectId Object, std::uint32_t Domains) {
			const auto Start = Clock::now();
			auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
			auto Root = FindRoot(InstanceValue);
			if (!Root) {
				StructureDirty = true;
				PendingDirtyMarkingNanoseconds += Nanoseconds(Clock::now() - Start);
				return;
			}
			RootDirty[Root->GetObjectId()] |= Domains;
			ObjectDirty[Object] |= Domains;
			if (!DirtyEpochs.contains(Object)) DirtyEpochs.emplace(Object, NextDirtyEpoch++);
			PendingDirtyMarkingNanoseconds += Nanoseconds(Clock::now() - Start);
		}

		void Observe(const std::shared_ptr<Instance> &Object) {
			if (!Object || !std::dynamic_pointer_cast<GuiBase>(Object)) return;
			const auto Id = Object->GetObjectId();
			if (Observers.contains(Id)) return;
			auto &Connections = Observers[Id];
			auto Connect = [&](std::string_view Name, std::uint32_t Domains) {
				if (!Object->FindProperty(std::string(Name))) return;
				Connections.push_back(Object->GetPropertyChangedSignal(std::string(Name))->Connect(
					[this, Id, Domains](std::monostate) { Mark(Id, Domains); }
				));
			};
			for (const auto *Name : {"Position", "AnchorPoint", "AutomaticSize", "Opacity", "Rotation", "ZIndex",
				"LayoutOrder", "ClipsDescendants", "FillDirection", "Padding"})
				Connect(Name, DirtyLayout | DirtyPresentation | DirtyInput);
			for (const auto *Name : {"CanvasSize", "AutomaticCanvasSize", "ScrollingDirection"})
				Connect(Name, DirtyLayout | DirtyPresentation | DirtyInput | DirtyAccessibility);
			for (const auto *Name : {"Visible", "Enabled", "DisplayOrder", "ClipToSafeArea", "ZIndexBehavior"})
				Connect(Name, DirtyLayout | DirtyPresentation | DirtyInput | DirtyAccessibility);
			Connect("CanvasPosition", DirtyScroll | DirtyPresentation | DirtyInput | DirtyAccessibility);
			Connect("Size", DirtyLayout | DirtyText | DirtyPresentation | DirtyInput);
			for (const auto *Name : {"BackgroundColor3", "BackgroundTransparency", "TextColor3", "TextTransparency",
				"ImageColor3", "ImageTransparency"}) Connect(Name, DirtyPresentation);
			for (const auto *Name : {"Text", "TextSize", "TextWrapped", "TextXAlignment", "TextYAlignment", "FontFace",
				"PlaceholderText", "SecureTextEntry", "MultiLine"})
				Connect(Name, DirtyLayout | DirtyText | DirtyPresentation | DirtyAccessibility);
			for (const auto *Name : {"PlaceholderColor3", "ScrollBarThickness"}) Connect(Name, DirtyPresentation);
			for (const auto *Name : {"ReadOnly", "MaxLength"}) Connect(Name, DirtyInput | DirtyAccessibility | DirtyPresentation);
			Connect("Image", DirtyLayout | DirtyPresentation | DirtyAccessibility);
			for (const auto *Name : {"Interactable", "InputSink", "Selectable"})
				Connect(Name, DirtyInput | DirtyAccessibility | DirtyPresentation);
			for (const auto *Name : {"AccessibleName", "AccessibilityRole", "AccessibilitySelected"})
				Connect(Name, DirtyAccessibility);
		}

		std::vector<std::shared_ptr<ScreenGui>> DiscoverRoots() {
			std::vector<std::shared_ptr<ScreenGui>> Roots;
			for (const auto &Object : World->GetDescendants()) {
				auto Root = std::dynamic_pointer_cast<ScreenGui>(Object);
				if (!Root) continue;
				if (Root->FindFirstAncestorWhichIsA("ScreenGui")) {
					AddDiagnostic("NestedRoot", "Nested ScreenGui roots are ignored", Root->GetObjectId());
					continue;
				}
				Roots.push_back(std::move(Root));
				if (Roots.size() == GuiLimits::MaximumRoots) break;
			}
			std::ranges::sort(Roots, [](const auto &Left, const auto &Right) {
				if (Left->GetDisplayOrder() != Right->GetDisplayOrder()) return Left->GetDisplayOrder() < Right->GetDisplayOrder();
				return Left->GetObjectId() < Right->GetObjectId();
			});
			return Roots;
		}

		GuiResolvedPresentation ResolvePresentation(const WorkingNode &Working) {
			GuiResolvedPresentation Result;
			const auto &Object = Working.Object;
			Result.BackgroundColor = Object->GetBackgroundColor3();
			Result.BackgroundAlpha = 1.0f - ClampUnit(Object->GetBackgroundTransparency());
			Result.Hovered = Object->GetGuiState() == Enums::GuiState::Hover;
			Result.Pressed = Object->GetGuiState() == Enums::GuiState::Press;
			Result.Enabled = Object->GetInteractable();
			Result.Selected = Object->GetAccessibilitySelected();
			if (auto Root = FindRoot(Object))
				Result.Focused = FocusedByRoot.contains(Root->GetObjectId()) &&
					FocusedByRoot.at(Root->GetObjectId()) == Object->GetObjectId();
			if (auto Input = std::dynamic_pointer_cast<TextBox>(Object)) {
				Result.Kind = GuiPresentationKind::TextInput;
				Result.ContentColor = Input->GetText().empty() && !Result.Focused ? Input->GetPlaceholderColor3() : Input->GetTextColor3();
				Result.ContentAlpha = 1.0f - ClampUnit(Input->GetTextTransparency());
				Result.Text = Working.Text;
				Result.Editable = true;
				Result.ReadOnly = Input->GetReadOnly();
				if (auto Editing = TextEditing.find(Object->GetObjectId()); Editing != TextEditing.end()) {
					auto &State = Editing->second;
					Result.DrawCaret = Result.Focused && !Input->GetReadOnly();
					if (Result.Text && !Result.Text->CaretOffsets.empty()) {
						const auto Caret = std::min(State.Caret, Result.Text->CaretOffsets.size() - 1);
						const auto Start = std::min(State.Caret, State.Anchor);
						const auto End = std::max(State.Caret, State.Anchor);
						Result.CaretX = Result.Text->CaretOffsets[Caret];
						const float VisibleWidth = std::max(1.0f, Working.Bounds.Width - 8.0f);
						if (!Input->GetMultiLine()) {
							if (Result.CaretX - State.HorizontalOffset > VisibleWidth)
								State.HorizontalOffset = Result.CaretX - VisibleWidth;
							else if (Result.CaretX - State.HorizontalOffset < 4.0f)
								State.HorizontalOffset = std::max(0.0f, Result.CaretX - 4.0f);
						}
						if (Start != End && End < Result.Text->CaretOffsets.size()) {
							Result.SelectionX = std::min(Result.Text->CaretOffsets[Start], Result.Text->CaretOffsets[End]);
							Result.SelectionWidth = std::abs(Result.Text->CaretOffsets[End] - Result.Text->CaretOffsets[Start]);
						}
					}
				}
			} else if (auto Button = std::dynamic_pointer_cast<TextButton>(Object)) {
				Result.Kind = GuiPresentationKind::Button;
				Result.ContentColor = Button->GetTextColor3();
				Result.ContentAlpha = 1.0f - ClampUnit(Button->GetTextTransparency());
				Result.Text = Working.Text;
			} else if (auto Label = std::dynamic_pointer_cast<TextLabel>(Object)) {
				Result.Kind = GuiPresentationKind::Text;
				Result.ContentColor = Label->GetTextColor3();
				Result.ContentAlpha = 1.0f - ClampUnit(Label->GetTextTransparency());
				Result.Text = Working.Text;
			} else if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object)) {
				Result.Kind = GuiPresentationKind::Image;
				Result.ContentColor = Image->GetImageColor3();
				Result.ContentAlpha = 1.0f - ClampUnit(Image->GetImageTransparency());
				if (Working.Image) Result.ImageTexture = Working.Image->Texture;
			} else if (std::dynamic_pointer_cast<ScrollingFrame>(Object)) {
				Result.Kind = GuiPresentationKind::ScrollView;
			} else if (std::dynamic_pointer_cast<Frame>(Object)) {
				Result.Kind = GuiPresentationKind::Rectangle;
			}
			if (Result.Text) {
				auto Label = std::dynamic_pointer_cast<TextLabel>(Object);
				if (Label->GetTextXAlignment() == Enums::TextXAlignment::Center)
					Result.TextOffsetX = (Working.Bounds.Width - Result.Text->Width) * 0.5f;
				else if (Label->GetTextXAlignment() == Enums::TextXAlignment::Right)
					Result.TextOffsetX = Working.Bounds.Width - Result.Text->Width;
				if (auto Input = std::dynamic_pointer_cast<TextBox>(Object))
					if (auto Editing = TextEditing.find(Input->GetObjectId()); Editing != TextEditing.end())
						Result.TextOffsetX -= Editing->second.HorizontalOffset;
				if (Label->GetTextYAlignment() == Enums::TextYAlignment::Center)
					Result.TextOffsetY = (Working.Bounds.Height - Result.Text->Height) * 0.5f;
				else if (Label->GetTextYAlignment() == Enums::TextYAlignment::Bottom)
					Result.TextOffsetY = Working.Bounds.Height - Result.Text->Height;
			}
			return Result;
		}

		bool CollectWorking(const std::shared_ptr<ScreenGui> &Root, std::vector<WorkingNode> &Working) {
			struct Pending { std::shared_ptr<Instance> Object; std::size_t Parent; std::uint32_t Depth; };
			std::vector<Pending> Stack;
			for (auto It = Root->Children.rbegin(); It != Root->Children.rend(); ++It) Stack.push_back({*It, NoParent, 1});
			std::unordered_set<ObjectId> Visited;
			std::uint32_t Order = 0;
			while (!Stack.empty()) {
				auto Current = std::move(Stack.back());
				Stack.pop_back();
				if (Current.Depth > GuiLimits::MaximumHierarchyDepth) {
					AddDiagnostic("TreeDepth", "GUI hierarchy depth exceeds the Foundation 1 bound", Root->GetObjectId());
					return false;
				}
				auto Object = std::dynamic_pointer_cast<GuiObject>(Current.Object);
				if (!Object) continue;
				const auto Id = Object->GetObjectId();
				if (!Visited.insert(Id).second) {
					AddDiagnostic("TreeCycle", "Malformed cyclic GUI hierarchy was rejected", Id);
					return false;
				}
				if (Working.size() >= GuiLimits::MaximumNodesPerRoot) {
					AddDiagnostic("NodeLimit", "GUI node count exceeds the Foundation 1 root bound", Root->GetObjectId());
					return false;
				}
				const std::size_t Index = Working.size();
				Working.push_back({Object, Current.Parent, Current.Depth, Order++});
				for (auto It = Object->Children.rbegin(); It != Object->Children.rend(); ++It)
					if (std::dynamic_pointer_cast<GuiObject>(*It)) Stack.push_back({*It, Index, Current.Depth + 1});
			}
			return true;
		}

		std::unordered_map<std::size_t, ListConfiguration> BuildLists(
			const std::shared_ptr<ScreenGui> &Root,
			const std::vector<WorkingNode> &Working
		) {
			std::unordered_map<std::size_t, ListConfiguration> Result;
			std::unordered_map<std::size_t, std::vector<std::size_t>> DirectChildren;
			for (std::size_t Index = 0; Index < Working.size(); ++Index)
				DirectChildren[Working[Index].Parent].push_back(Index);
			auto Inspect = [&](std::size_t ParentIndex, const std::vector<std::shared_ptr<Instance>> &Children) {
				std::shared_ptr<UIListLayout> LayoutValue;
				for (const auto &Child : Children) {
					if (auto Candidate = std::dynamic_pointer_cast<UIListLayout>(Child)) {
						if (!LayoutValue) LayoutValue = std::move(Candidate);
						else AddDiagnostic("MultipleLayouts", "Only the first UIListLayout under a parent is active", Child->GetObjectId());
					}
				}
				if (!LayoutValue) return;
				ListConfiguration Configuration{LayoutValue->GetFillDirection(), LayoutValue->GetPadding(), {}};
				if (auto Existing = DirectChildren.find(ParentIndex); Existing != DirectChildren.end())
					Configuration.Children = Existing->second;
				std::ranges::stable_sort(Configuration.Children, [&](std::size_t Left, std::size_t Right) {
					const int LeftOrder = Working[Left].Object->GetLayoutOrder();
					const int RightOrder = Working[Right].Object->GetLayoutOrder();
					return LeftOrder != RightOrder ? LeftOrder < RightOrder : Working[Left].OriginalOrder < Working[Right].OriginalOrder;
				});
				Result.emplace(ParentIndex, std::move(Configuration));
			};
			Inspect(NoParent, Root->Children);
			for (std::size_t Index = 0; Index < Working.size(); ++Index) Inspect(Index, Working[Index].Object->Children);
			return Result;
		}

		bool ArrangePass(
			const std::shared_ptr<ScreenGui> &Root,
			std::vector<WorkingNode> &Working,
			const std::unordered_map<std::size_t, ListConfiguration> &Lists,
			bool UseAutomatic,
			bool &Changed
		) {
			const GuiRect RootBounds{
				Viewport.SafeArea.Left,
				Viewport.SafeArea.Top,
				Viewport.LogicalWidth() - Viewport.SafeArea.Left - Viewport.SafeArea.Right,
				Viewport.LogicalHeight() - Viewport.SafeArea.Top - Viewport.SafeArea.Bottom,
			};
			for (auto &Node : Working) {
				const auto ParentBounds = Node.Parent == NoParent ? RootBounds : Working[Node.Parent].Bounds;
				const auto Size = Node.Object->GetSize();
				const auto Automatic = Node.Object->GetAutomaticSize();
				float Width = std::max(0.0f, Resolve(Size.X, ParentBounds.Width));
				float Height = std::max(0.0f, Resolve(Size.Y, ParentBounds.Height));
				if (UseAutomatic && (Automatic == Enums::AutomaticSize::X || Automatic == Enums::AutomaticSize::XY) &&
					std::isfinite(Node.AutoWidth)) Width = Node.AutoWidth;
				if (UseAutomatic && (Automatic == Enums::AutomaticSize::Y || Automatic == Enums::AutomaticSize::XY) &&
					std::isfinite(Node.AutoHeight)) Height = Node.AutoHeight;
				Node.Bounds.Width = Width;
				Node.Bounds.Height = Height;
			}

			std::unordered_map<std::size_t, float> ListOffsets;
			for (const auto &[ParentIndex, List] : Lists) {
				const auto ParentBounds = ParentIndex == NoParent ? RootBounds : Working[ParentIndex].Bounds;
				const float ParentMain = List.Direction == Enums::GuiFillDirection::Vertical ? ParentBounds.Height : ParentBounds.Width;
				const float Padding = Resolve(List.Padding, ParentMain);
				float Cursor = 0.0f;
				for (const auto Child : List.Children) {
					ListOffsets[Child] = Cursor;
					Cursor += (List.Direction == Enums::GuiFillDirection::Vertical ? Working[Child].Bounds.Height : Working[Child].Bounds.Width) + Padding;
				}
			}

			for (std::size_t Index = 0; Index < Working.size(); ++Index) {
				auto &Node = Working[Index];
				const auto ParentBounds = Node.Parent == NoParent ? RootBounds : Working[Node.Parent].Bounds;
				const auto Position = Node.Object->GetPosition();
				const auto Anchor = Node.Object->GetAnchorPoint();
				Node.Bounds.X = ParentBounds.X + Resolve(Position.X, ParentBounds.Width) - Anchor.GetX() * Node.Bounds.Width;
				Node.Bounds.Y = ParentBounds.Y + Resolve(Position.Y, ParentBounds.Height) - Anchor.GetY() * Node.Bounds.Height;
				if (auto List = Lists.find(Node.Parent); List != Lists.end()) {
					if (List->second.Direction == Enums::GuiFillDirection::Vertical) Node.Bounds.Y = ParentBounds.Y + ListOffsets[Index];
					else Node.Bounds.X = ParentBounds.X + ListOffsets[Index];
				}
				if (Node.Parent != NoParent) {
					if (auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(Working[Node.Parent].Object)) {
						const auto Extent = Scroll->GetContentExtent();
						const auto Canvas = Scroll->GetCanvasPosition();
						const float MaximumX = std::max(0.0f, Extent.GetX() - ParentBounds.Width);
						const float MaximumY = std::max(0.0f, Extent.GetY() - ParentBounds.Height);
						Node.Bounds.X -= std::clamp(Canvas.GetX(), 0.0f, MaximumX);
						Node.Bounds.Y -= std::clamp(Canvas.GetY(), 0.0f, MaximumY);
					}
				}
			}

			const auto MeasureStart = Clock::now();
			for (auto &Node : Working) {
				float DesiredWidth = Node.Bounds.Width;
				float DesiredHeight = Node.Bounds.Height;
				if (auto Label = std::dynamic_pointer_cast<TextLabel>(Node.Object)) {
					std::string DisplayText = Label->GetText();
					bool EditableMetrics = false;
					if (auto Input = std::dynamic_pointer_cast<TextBox>(Label)) {
						EditableMetrics = true;
						auto Normalized = NormalizeEditableUtf8(DisplayText);
						if (Normalized.Bytes != DisplayText) {
							Input->SetText(Normalized.Bytes);
							DisplayText = std::move(Normalized.Bytes);
							AddDiagnostic("InvalidEditableUtf8",
								Normalized.Replaced ? "Invalid editable UTF-8 was replaced with U+FFFD" :
								"Editable text was truncated at its byte or code-point bound",
								Input->GetObjectId());
						}
						const auto RootId = Root->GetObjectId();
						const bool Focused = FocusedByRoot.contains(RootId) && FocusedByRoot.at(RootId) == Input->GetObjectId();
						if (auto Editing = TextEditing.find(Input->GetObjectId()); Editing != TextEditing.end() &&
							!Editing->second.Composition.empty()) {
							const auto Boundaries = Utf8Boundaries(DisplayText);
							const auto Position = Boundaries[std::min(Editing->second.Caret, Boundaries.size() - 1)];
							DisplayText.insert(Position, Editing->second.Composition);
						}
						if (Input->GetSecureTextEntry()) DisplayText = MaskEditableText(DisplayText);
						else if (DisplayText.empty() && !Focused) DisplayText = Input->GetPlaceholderText();
					}
					Node.Text = Text.Shape({
						DisplayText, Label->GetFontFace(), Label->GetTextSize(), Viewport.DpiScale,
						Node.Bounds.Width, Label->GetTextWrapped(), static_cast<int>(Label->GetTextXAlignment()), EditableMetrics
					});
					if (Node.Text && Node.Text->ReplacedInvalidUtf8)
						AddDiagnostic("InvalidUtf8", "Invalid UTF-8 was replaced with U+FFFD", Node.Object->GetObjectId());
					if (Node.Text && Node.Text->TruncatedInput)
						AddDiagnostic("TextLimit", "GUI text was truncated at the Foundation 1 byte or glyph bound", Node.Object->GetObjectId());
					if (Node.Text && Node.Text->UsedMissingGlyph)
						AddDiagnostic("MissingGlyph", "The controlled GUI font used its replacement glyph", Node.Object->GetObjectId());
					if (Node.Text && Node.Text->ResourceLimit) {
						AddDiagnostic("GlyphResourceLimit", "GUI glyph rasterization or atlas allocation reached a bounded failure", Node.Object->GetObjectId());
						return false;
					}
					if (Node.Text && MeasuredTextThisFrame.insert(Node.Object->GetObjectId()).second) {
						if (Node.Text->Glyphs.size() > GuiLimits::MaximumShapedGlyphsPerFrame - ShapedGlyphsThisFrame) {
							AddDiagnostic("ShapedGlyphLimit", "GUI shaped glyph work exceeds the per-frame Foundation 1 bound", Root->GetObjectId());
							return false;
						}
						ShapedGlyphsThisFrame += Node.Text->Glyphs.size();
					}
					DesiredWidth = Node.Text ? Node.Text->Width : 0.0f;
					DesiredHeight = Node.Text ? Node.Text->Height : 0.0f;
				} else if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Node.Object)) {
					Node.Image = Assets->ResolveImage(Image->GetImage());
					if (Node.Image) {
						DesiredWidth = static_cast<float>(Node.Image->Width) / Viewport.DpiScale;
						DesiredHeight = static_cast<float>(Node.Image->Height) / Viewport.DpiScale;
					}
				}
				const auto Automatic = Node.Object->GetAutomaticSize();
				if (Automatic == Enums::AutomaticSize::X || Automatic == Enums::AutomaticSize::XY) {
					if (!std::isfinite(Node.AutoWidth) || std::abs(Node.AutoWidth - DesiredWidth) > 0.01f) Changed = true;
					Node.AutoWidth = std::max(0.0f, DesiredWidth);
				}
				if (Automatic == Enums::AutomaticSize::Y || Automatic == Enums::AutomaticSize::XY) {
					if (!std::isfinite(Node.AutoHeight) || std::abs(Node.AutoHeight - DesiredHeight) > 0.01f) Changed = true;
					Node.AutoHeight = std::max(0.0f, DesiredHeight);
				}
			}

			std::vector<float> ContentWidths(Working.size());
			std::vector<float> ContentHeights(Working.size());
			for (const auto &Child : Working) {
				if (Child.Parent == NoParent || !Child.Object->GetVisible()) continue;
				const auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(Working[Child.Parent].Object);
				const auto Canvas = Scroll ? Scroll->GetCanvasPosition() : Vector2{};
				ContentWidths[Child.Parent] = std::max(ContentWidths[Child.Parent],
					Child.Bounds.X + Canvas.GetX() + Child.Bounds.Width - Working[Child.Parent].Bounds.X);
				ContentHeights[Child.Parent] = std::max(ContentHeights[Child.Parent],
					Child.Bounds.Y + Canvas.GetY() + Child.Bounds.Height - Working[Child.Parent].Bounds.Y);
			}
			for (std::size_t Reverse = Working.size(); Reverse-- > 0;) {
				auto &Node = Working[Reverse];
				if (auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(Node.Object)) {
					const auto Authored = Scroll->GetCanvasSize();
					float ExtentX = std::max(Node.Bounds.Width, Resolve(Authored.X, Node.Bounds.Width));
					float ExtentY = std::max(Node.Bounds.Height, Resolve(Authored.Y, Node.Bounds.Height));
					const auto AutomaticCanvas = Scroll->GetAutomaticCanvasSize();
					if (AutomaticCanvas == Enums::AutomaticSize::X || AutomaticCanvas == Enums::AutomaticSize::XY)
						ExtentX = std::max(ExtentX, ContentWidths[Reverse]);
					if (AutomaticCanvas == Enums::AutomaticSize::Y || AutomaticCanvas == Enums::AutomaticSize::XY)
						ExtentY = std::max(ExtentY, ContentHeights[Reverse]);
					ExtentX = std::min(ExtentX, GuiLimits::MaximumScrollExtent);
					ExtentY = std::min(ExtentY, GuiLimits::MaximumScrollExtent);
					const auto Previous = Scroll->GetContentExtent();
					if (std::abs(Previous.GetX() - ExtentX) > 0.01f || std::abs(Previous.GetY() - ExtentY) > 0.01f)
						Changed = true;
					Scroll->CommitRuntimeContentExtent({ExtentX, ExtentY});
				}
				if (std::dynamic_pointer_cast<TextLabel>(Node.Object) || std::dynamic_pointer_cast<ImageLabel>(Node.Object)) continue;
				const float ContentWidth = ContentWidths[Reverse];
				const float ContentHeight = ContentHeights[Reverse];
				const auto Automatic = Node.Object->GetAutomaticSize();
				if (Automatic == Enums::AutomaticSize::X || Automatic == Enums::AutomaticSize::XY) {
					if (!std::isfinite(Node.AutoWidth) || std::abs(Node.AutoWidth - ContentWidth) > 0.01f) Changed = true;
					Node.AutoWidth = std::max(0.0f, ContentWidth);
				}
				if (Automatic == Enums::AutomaticSize::Y || Automatic == Enums::AutomaticSize::XY) {
					if (!std::isfinite(Node.AutoHeight) || std::abs(Node.AutoHeight - ContentHeight) > 0.01f) Changed = true;
					Node.AutoHeight = std::max(0.0f, ContentHeight);
				}
			}
			Profile.MeasureNanoseconds += Nanoseconds(Clock::now() - MeasureStart);
			return true;
		}

		std::optional<RootCache> BuildRoot(const std::shared_ptr<ScreenGui> &Root) {
			std::vector<WorkingNode> Working;
			if (!CollectWorking(Root, Working)) return std::nullopt;
			const auto Lists = BuildLists(Root, Working);
			for (std::size_t Index = 0; Index < Working.size(); ++Index) {
				std::size_t ScrollDepth = 0;
				for (std::size_t Parent = Index; Parent != NoParent; Parent = Working[Parent].Parent)
					if (std::dynamic_pointer_cast<ScrollingFrame>(Working[Parent].Object)) ++ScrollDepth;
				if (ScrollDepth > GuiLimits::MaximumScrollNesting) {
					AddDiagnostic("ScrollDepth", "GUI scroll nesting exceeds the Foundation 2 bound", Working[Index].Object->GetObjectId());
					return std::nullopt;
				}
			}
			bool Converged = false;
			for (std::size_t Pass = 0; Pass + 1 < GuiLimits::MaximumAutomaticSizePasses; ++Pass) {
				if (LayoutPassesThisFrame >= GuiLimits::MaximumLayoutPassesPerFrame) {
					AddDiagnostic("LayoutPassLimit", "GUI layout exhausted the per-frame Foundation 1 pass bound", Root->GetObjectId());
					return std::nullopt;
				}
				++LayoutPassesThisFrame;
				bool Changed = false;
				const auto ArrangeStart = Clock::now();
				if (!ArrangePass(Root, Working, Lists, true, Changed)) return std::nullopt;
				Profile.ArrangeNanoseconds += Nanoseconds(Clock::now() - ArrangeStart);
				if (!Changed) {
					Converged = true;
					break;
				}
			}
			if (!Converged) {
				AddDiagnostic("LayoutConvergence", "AutomaticSize did not converge; authored sizes were committed as a bounded fallback", Root->GetObjectId());
				if (LayoutPassesThisFrame >= GuiLimits::MaximumLayoutPassesPerFrame) return std::nullopt;
				++LayoutPassesThisFrame;
				bool Ignored = false;
				if (!ArrangePass(Root, Working, Lists, false, Ignored)) return std::nullopt;
			}

			RootCache Cache;
			Cache.Root = Root->GetObjectId();
			Cache.DisplayOrder = Root->GetDisplayOrder();
			Cache.ZIndexBehavior = Root->GetZIndexBehavior();
			Cache.AutomaticFallback = !Converged;
			Cache.HasDependentLayout = !Lists.empty() || std::ranges::any_of(Working, [](const WorkingNode &Node) {
				return Node.Object->GetAutomaticSize() != Enums::AutomaticSize::None ||
					std::dynamic_pointer_cast<ScrollingFrame>(Node.Object) != nullptr;
			});
			const GuiRect RootBounds{
				Viewport.SafeArea.Left, Viewport.SafeArea.Top,
				Viewport.LogicalWidth() - Viewport.SafeArea.Left - Viewport.SafeArea.Right,
				Viewport.LogicalHeight() - Viewport.SafeArea.Top - Viewport.SafeArea.Bottom,
			};
			GuiLayoutNode RootNode;
			RootNode.Object = Cache.Root;
			RootNode.Root = Cache.Root;
			RootNode.Bounds = RootBounds;
			RootNode.TransformedBounds = RootBounds;
			RootNode.EffectiveClip = Root->GetClipToSafeArea() ? std::optional(RootBounds) : std::nullopt;
			RootNode.EffectiveVisible = Root->GetEnabled();
			RootNode.EffectiveOpacity = ClampUnit(Root->GetOpacity());
			RootNode.EffectiveLayer = static_cast<std::int64_t>(Root->GetDisplayOrder()) * 1'000'000;
			RootNode.TreeOrder = 0;
			RootNode.Depth = 0;
			Cache.Nodes.push_back(RootNode);
			Root->CommitRuntimeGeometry({RootBounds.X, RootBounds.Y}, {RootBounds.Width, RootBounds.Height}, 0.0f);

			for (std::size_t Index = 0; Index < Working.size(); ++Index) {
				auto &Node = Working[Index];
				const GuiTransform ParentTransform = Node.Parent == NoParent ? GuiTransform{} : Working[Node.Parent].Transform;
				Node.Transform = ParentTransform.Then(GuiTransform::RotationAbout(
					Node.Object->GetRotation(), Node.Bounds.X + Node.Bounds.Width * 0.5f, Node.Bounds.Y + Node.Bounds.Height * 0.5f
				));
				Node.TransformedBounds = TransformBounds(Node.Transform, Node.Bounds);
				const auto &ParentCommitted = Node.Parent == NoParent ? Cache.Nodes.front() : Cache.Nodes[Node.Parent + 1];
				Node.Clip = ParentCommitted.EffectiveClip;
				std::size_t ClipDepth = 0;
				for (std::size_t Parent = Node.Parent; Parent != NoParent; Parent = Working[Parent].Parent)
					if (Working[Parent].Object->GetClipsDescendants()) ++ClipDepth;
				if (Root->GetClipToSafeArea()) ++ClipDepth;
				if (ClipDepth > GuiLimits::MaximumClipDepth) {
					AddDiagnostic("ClipDepth", "GUI clip nesting exceeds the Foundation 1 bound", Node.Object->GetObjectId());
					return std::nullopt;
				}
				if (Node.Parent != NoParent && Working[Node.Parent].Object->GetClipsDescendants()) {
					const auto ParentBounds = Working[Node.Parent].TransformedBounds;
					Node.Clip = Node.Clip ? GuiRect::Intersect(*Node.Clip, ParentBounds) : std::optional(ParentBounds);
				}

				GuiLayoutNode Committed;
				Committed.Object = Node.Object->GetObjectId();
				Committed.Parent = Node.Parent == NoParent ? Cache.Root : Working[Node.Parent].Object->GetObjectId();
				Committed.Root = Cache.Root;
				Committed.Bounds = Node.Bounds;
				Committed.TransformedBounds = Node.TransformedBounds;
				Committed.Transform = Node.Transform;
				Committed.EffectiveClip = Node.Clip;
				Committed.EffectiveLayer = static_cast<std::int64_t>(Root->GetDisplayOrder()) * 1'000'000 + Node.Object->GetZIndex();
				Committed.TreeOrder = static_cast<std::uint32_t>(Index + 1);
				Committed.Depth = Node.Depth;
				Committed.LayoutOrder = Node.Object->GetLayoutOrder();
				Committed.EffectiveVisible = ParentCommitted.EffectiveVisible && Node.Object->GetVisible() &&
					(!Node.Clip || !Node.Clip->IsEmpty());
				Committed.EffectiveOpacity = ParentCommitted.EffectiveOpacity * ClampUnit(Node.Object->GetOpacity());
				Committed.Interactable = Node.Object->GetInteractable();
				Committed.FocusEligible = Node.Object->GetInteractable() && Node.Object->GetSelectable();
				Committed.InputSink = Node.Object->GetInputSink();
				if (auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(Node.Object)) {
					Committed.ScrollContainer = true;
					Committed.ContentExtent = Scroll->GetContentExtent();
					const auto Canvas = Scroll->GetCanvasPosition();
					Committed.ScrollOffset = {
						std::clamp(Canvas.GetX(), 0.0f, std::max(0.0f, Committed.ContentExtent.GetX() - Node.Bounds.Width)),
						std::clamp(Canvas.GetY(), 0.0f, std::max(0.0f, Committed.ContentExtent.GetY() - Node.Bounds.Height)),
					};
				}
				Committed.Presentation = ResolvePresentation(Node);
				Cache.Nodes.push_back(std::move(Committed));
				Node.Object->CommitRuntimeGeometry(
					{Node.Bounds.X, Node.Bounds.Y}, {Node.Bounds.Width, Node.Bounds.Height}, Node.Object->GetRotation()
				);
			}
			Cache.NodeByObject.reserve(Cache.Nodes.size());
			for (std::size_t Index = 0; Index < Cache.Nodes.size(); ++Index)
				Cache.NodeByObject.emplace(Cache.Nodes[Index].Object, Index);
			++Profile.LayoutRoots;
			Profile.LayoutNodes += Cache.Nodes.size();
			return Cache;
		}

		struct RefreshResult {
			bool SnapshotChanged = false;
			bool VisualChanged = false;
			bool AccessibilityChanged = false;
		};

		RefreshResult RefreshRoot(RootCache &Cache, const std::unordered_map<ObjectId, std::uint32_t> &DirtyObjects) {
			const auto Start = Clock::now();
			RefreshResult Result;
			for (const auto &[ObjectIdValue, Domains] : DirtyObjects) {
				auto Cached = Cache.NodeByObject.find(ObjectIdValue);
				if (Cached == Cache.NodeByObject.end() || ObjectIdValue == Cache.Root) continue;
				auto &Node = Cache.Nodes[Cached->second];
				auto Object = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(ObjectIdValue));
				if (!Object) continue;
				Result.SnapshotChanged = Result.SnapshotChanged || (Domains & DirtyInput) != 0;
				Result.AccessibilityChanged = Result.AccessibilityChanged || (Domains & DirtyAccessibility) != 0;
				if ((Domains & DirtyInput) != 0) {
					Node.Interactable = Object->GetInteractable();
					Node.FocusEligible = Object->GetInteractable() && Object->GetSelectable();
					Node.InputSink = Object->GetInputSink();
					if (!Node.Interactable) {
						std::erase_if(Pressed, [&](const auto &Entry) { return Entry.second == ObjectIdValue; });
						if (KeyboardPressed == ObjectIdValue) KeyboardPressed = {};
					}
					RefreshInteraction(ObjectIdValue);
				}
				if ((Domains & DirtyPresentation) != 0) {
					const auto Previous = Node.Presentation;
					WorkingNode Working;
					Working.Object = Object;
					Working.Bounds = Node.Bounds;
					Working.Text = Node.Presentation.Text;
					if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object)) Working.Image = Assets->ResolveImage(Image->GetImage());
					Node.Presentation = ResolvePresentation(Working);
					if (!SameVisualPresentation(Previous, Node.Presentation)) {
						Result.VisualChanged = true;
						PendingVisualObjects.insert(ObjectIdValue);
					}
					++Profile.PresentationNodes;
				}
			}
			Profile.PresentationResolutionNanoseconds += Nanoseconds(Clock::now() - Start);
			return Result;
		}

		const GuiResolvedPresentation &CommittedPresentation(const GuiLayoutNode &Node) const {
			if (auto Cache = RootCaches.find(Node.Root); Cache != RootCaches.end()) {
				if (auto Existing = Cache->second.NodeByObject.find(Node.Object); Existing != Cache->second.NodeByObject.end())
					return Cache->second.Nodes[Existing->second].Presentation;
			}
			return Node.Presentation;
		}

		std::optional<RefreshResult> RefreshLayout(
			RootCache &Cache,
			const std::unordered_map<ObjectId, std::uint32_t> &DirtyObjects
		) {
			if (Cache.HasDependentLayout || DirtyObjects.empty()) return std::nullopt;
			if (auto RootDirtyValue = DirtyObjects.find(Cache.Root); RootDirtyValue != DirtyObjects.end() &&
				(RootDirtyValue->second & DirtyLayout) != 0) return std::nullopt;
			RefreshResult Result;
			std::vector<bool> Affected(Cache.Nodes.size(), false);
			for (std::size_t Index = 1; Index < Cache.Nodes.size(); ++Index) {
				const auto &Existing = Cache.Nodes[Index];
				const auto Own = DirtyObjects.find(Existing.Object);
				const bool OwnLayout = Own != DirtyObjects.end() && (Own->second & DirtyLayout) != 0;
				const auto Parent = Cache.NodeByObject.find(Existing.Parent);
				Affected[Index] = OwnLayout || (Parent != Cache.NodeByObject.end() && Affected[Parent->second]);
				if (!Affected[Index]) continue;
				auto Object = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Existing.Object));
				if (!Object) return std::nullopt;
				const auto ParentIndex = Cache.NodeByObject.find(Existing.Parent);
				if (ParentIndex == Cache.NodeByObject.end()) return std::nullopt;
				const auto &ParentNode = Cache.Nodes[ParentIndex->second];
				const auto Size = Object->GetSize();
				const auto Position = Object->GetPosition();
				const auto Anchor = Object->GetAnchorPoint();
				GuiRect Bounds;
				Bounds.Width = std::max(0.0f, Resolve(Size.X, ParentNode.Bounds.Width));
				Bounds.Height = std::max(0.0f, Resolve(Size.Y, ParentNode.Bounds.Height));
				Bounds.X = ParentNode.Bounds.X + Resolve(Position.X, ParentNode.Bounds.Width) - Anchor.GetX() * Bounds.Width;
				Bounds.Y = ParentNode.Bounds.Y + Resolve(Position.Y, ParentNode.Bounds.Height) - Anchor.GetY() * Bounds.Height;
				const auto Transform = ParentNode.Transform.Then(GuiTransform::RotationAbout(
					Object->GetRotation(), Bounds.X + Bounds.Width * 0.5f, Bounds.Y + Bounds.Height * 0.5f));
				auto Clip = ParentNode.EffectiveClip;
				if (auto ParentObject = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Existing.Parent));
					ParentObject && ParentObject->GetClipsDescendants()) {
					const auto ParentBounds = ParentNode.TransformedBounds;
					Clip = Clip ? GuiRect::Intersect(*Clip, ParentBounds) : std::optional(ParentBounds);
				}
				auto Next = Existing;
				Next.Bounds = Bounds;
				Next.Transform = Transform;
				Next.TransformedBounds = TransformBounds(Transform, Bounds);
				Next.EffectiveClip = Clip;
				Next.EffectiveLayer = static_cast<std::int64_t>(Cache.DisplayOrder) * 1'000'000 + Object->GetZIndex();
				Next.LayoutOrder = Object->GetLayoutOrder();
				Next.EffectiveVisible = ParentNode.EffectiveVisible && Object->GetVisible() && (!Clip || !Clip->IsEmpty());
				Next.EffectiveOpacity = ParentNode.EffectiveOpacity * ClampUnit(Object->GetOpacity());
				Next.Interactable = Object->GetInteractable();
				Next.FocusEligible = Object->GetInteractable() && Object->GetSelectable();
				Next.InputSink = Object->GetInputSink();
				WorkingNode Working;
				Working.Object = Object;
				Working.Bounds = Bounds;
				Working.Text = Existing.Presentation.Text;
				if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object)) Working.Image = Assets->ResolveImage(Image->GetImage());
				Next.Presentation = ResolvePresentation(Working);
				const bool NodeVisualChanged = !SameVisualPresentation(Existing.Presentation, Next.Presentation) ||
					Existing.Bounds.X != Next.Bounds.X || Existing.Bounds.Y != Next.Bounds.Y ||
					Existing.Bounds.Width != Next.Bounds.Width || Existing.Bounds.Height != Next.Bounds.Height ||
					Existing.Transform.M00 != Next.Transform.M00 || Existing.Transform.M01 != Next.Transform.M01 ||
					Existing.Transform.M10 != Next.Transform.M10 || Existing.Transform.M11 != Next.Transform.M11;
				if (NodeVisualChanged) {
					Result.VisualChanged = true;
					PendingVisualObjects.insert(Existing.Object);
				}
				Result.AccessibilityChanged = Result.AccessibilityChanged || Existing.EffectiveVisible != Next.EffectiveVisible ||
					(Own != DirtyObjects.end() && (Own->second & DirtyAccessibility) != 0);
				Result.SnapshotChanged = true;
				Cache.Nodes[Index] = std::move(Next);
				Object->CommitRuntimeGeometry({Bounds.X, Bounds.Y}, {Bounds.Width, Bounds.Height}, Object->GetRotation());
				++Profile.LayoutNodes;
			}
			++Profile.LayoutRoots;
			return Result;
		}

		RefreshResult RefreshScroll(RootCache &Cache, const std::unordered_map<ObjectId, std::uint32_t> &DirtyObjects) {
			RefreshResult Result;
			for (const auto &[ObjectIdValue, Domains] : DirtyObjects) {
				if ((Domains & DirtyScroll) == 0) continue;
				auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(ObjectRegistry::Get().Lookup(ObjectIdValue));
				auto Cached = Cache.NodeByObject.find(ObjectIdValue);
				if (!Scroll || Cached == Cache.NodeByObject.end()) continue;
				auto &ScrollNode = Cache.Nodes[Cached->second];
				const auto Authored = Scroll->GetCanvasPosition();
				const Vector2 NextOffset{
					std::clamp(Authored.GetX(), 0.0f, std::max(0.0f, ScrollNode.ContentExtent.GetX() - ScrollNode.Bounds.Width)),
					std::clamp(Authored.GetY(), 0.0f, std::max(0.0f, ScrollNode.ContentExtent.GetY() - ScrollNode.Bounds.Height)),
				};
				const auto Delta = ScrollNode.ScrollOffset - NextOffset;
				if (Delta.GetX() == 0.0f && Delta.GetY() == 0.0f) continue;
				ScrollNode.ScrollOffset = NextOffset;
				const auto ScrollDepth = ScrollNode.Depth;
				for (std::size_t Index = Cached->second + 1; Index < Cache.Nodes.size(); ++Index) {
					auto &Node = Cache.Nodes[Index];
					if (Node.Depth <= ScrollDepth) break;
					Node.Bounds.X += Delta.GetX();
					Node.Bounds.Y += Delta.GetY();
					Node.Transform.Tx += Delta.GetX();
					Node.Transform.Ty += Delta.GetY();
					Node.TransformedBounds.X += Delta.GetX();
					Node.TransformedBounds.Y += Delta.GetY();
					const auto Parent = Cache.NodeByObject.find(Node.Parent);
					if (Parent != Cache.NodeByObject.end()) {
						const auto &ParentNode = Cache.Nodes[Parent->second];
						Node.EffectiveClip = ParentNode.EffectiveClip;
						if (auto ParentObject = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Node.Parent));
							ParentObject && ParentObject->GetClipsDescendants())
							Node.EffectiveClip = Node.EffectiveClip ? GuiRect::Intersect(*Node.EffectiveClip, ParentNode.TransformedBounds) :
								std::optional(ParentNode.TransformedBounds);
						auto Object = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Node.Object));
						Node.EffectiveVisible = ParentNode.EffectiveVisible && Object && Object->GetVisible() &&
							(!Node.EffectiveClip || !Node.EffectiveClip->IsEmpty());
					}
					if (auto Object = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Node.Object)))
						Object->CommitRuntimeGeometry({Node.Bounds.X, Node.Bounds.Y}, {Node.Bounds.Width, Node.Bounds.Height}, Object->GetRotation());
					PendingVisualObjects.insert(Node.Object);
					++Profile.LayoutNodes;
				}
				Result.SnapshotChanged = true;
				Result.VisualChanged = true;
				Result.AccessibilityChanged = true;
				++Profile.LayoutRoots;
			}
			return Result;
		}

		void MergeSnapshots(const std::vector<std::shared_ptr<ScreenGui>> &Roots) {
			auto Next = std::make_shared<GuiLayoutSnapshot>();
			Next->Generation = NextLayoutGeneration++;
			Next->Viewport = Viewport;
			std::uint32_t PaintOrder = 0;
			for (const auto &Root : Roots) {
				auto Cache = RootCaches.find(Root->GetObjectId());
				if (Cache == RootCaches.end()) continue;
				auto &RootCacheValue = Cache->second;
				std::vector<std::size_t> Order;
				Order.reserve(RootCacheValue.Nodes.size());
				if (RootCacheValue.ZIndexBehavior == Enums::ZIndexBehavior::Global) {
					if (!RootCacheValue.Nodes.empty()) Order.push_back(0);
					for (std::size_t Index = 1; Index < RootCacheValue.Nodes.size(); ++Index) Order.push_back(Index);
					std::ranges::stable_sort(Order.begin() + std::min<std::size_t>(1, Order.size()), Order.end(),
						[&](std::size_t Left, std::size_t Right) {
							const auto &LeftNode = RootCacheValue.Nodes[Left];
							const auto &RightNode = RootCacheValue.Nodes[Right];
							return LeftNode.EffectiveLayer != RightNode.EffectiveLayer ?
								LeftNode.EffectiveLayer < RightNode.EffectiveLayer : LeftNode.TreeOrder < RightNode.TreeOrder;
						});
				} else {
					std::unordered_map<ObjectId, std::vector<std::size_t>> Children;
					for (std::size_t Index = 1; Index < RootCacheValue.Nodes.size(); ++Index)
						Children[RootCacheValue.Nodes[Index].Parent].push_back(Index);
					for (auto &[Parent, Siblings] : Children) {
						(void)Parent;
						std::ranges::stable_sort(Siblings, [&](std::size_t Left, std::size_t Right) {
							const auto &LeftNode = RootCacheValue.Nodes[Left];
							const auto &RightNode = RootCacheValue.Nodes[Right];
							return LeftNode.EffectiveLayer != RightNode.EffectiveLayer ?
								LeftNode.EffectiveLayer < RightNode.EffectiveLayer : LeftNode.TreeOrder < RightNode.TreeOrder;
						});
					}
					if (!RootCacheValue.Nodes.empty()) Order.push_back(0);
					std::vector<std::size_t> Stack;
					if (auto Direct = Children.find(RootCacheValue.Root); Direct != Children.end())
						for (auto It = Direct->second.rbegin(); It != Direct->second.rend(); ++It) Stack.push_back(*It);
					while (!Stack.empty()) {
						const auto Index = Stack.back();
						Stack.pop_back();
						Order.push_back(Index);
						if (auto Direct = Children.find(RootCacheValue.Nodes[Index].Object); Direct != Children.end())
							for (auto It = Direct->second.rbegin(); It != Direct->second.rend(); ++It) Stack.push_back(*It);
					}
				}
				for (const auto Index : Order) {
					auto Node = RootCacheValue.Nodes[Index];
					Node.PaintOrder = PaintOrder++;
					Next->Nodes.push_back(std::move(Node));
				}
			}
			Next->NodeByObject.reserve(Next->Nodes.size());
			for (std::size_t Index = 0; Index < Next->Nodes.size(); ++Index) Next->NodeByObject.emplace(Next->Nodes[Index].Object, Index);
			Layout = std::shared_ptr<const GuiLayoutSnapshot>(std::move(Next));
		}

		bool AddQuad(
			RenderUiFrame &FrameValue,
			const GuiLayoutNode &Node,
			const GuiRect &Bounds,
			const GuiRect &Uv,
			const Color3 &Color,
			float Alpha,
			std::optional<RenderTextureIdentity> Texture,
			std::optional<GuiRect> LogicalClip,
			PresentationSpan *Span = nullptr
		) {
			std::optional<RenderUiClipRect> Clip;
			if (LogicalClip) Clip = RenderUiClipRect{
				LogicalClip->X * Viewport.DpiScale, LogicalClip->Y * Viewport.DpiScale,
				LogicalClip->Width * Viewport.DpiScale, LogicalClip->Height * Viewport.DpiScale};
			const std::int32_t LayerValue = static_cast<std::int32_t>(std::clamp<std::int64_t>(
				Node.EffectiveLayer, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
			RenderUiBatch *Batch = nullptr;
			if (!FrameValue.Batches.empty()) {
				auto &Candidate = FrameValue.Batches.back();
				if (Candidate.Texture == Texture && SameClip(Candidate.Clip, Clip) && Candidate.Layer == LayerValue &&
					Candidate.Opacity == Node.EffectiveOpacity) Batch = &Candidate;
			}
			if (!Batch) {
				FrameValue.Batches.push_back({Texture, Clip, LayerValue, Node.EffectiveOpacity, {}, {}});
				Batch = &FrameValue.Batches.back();
			}
			if (Profile.DisplayPrimitives >= GuiLimits::MaximumDisplayPrimitives ||
				PresentationVertices + 4 > GuiLimits::MaximumUiVertices ||
				PresentationIndices + 6 > GuiLimits::MaximumUiIndices)
				return false;
			const std::array<Vector2, 4> Logical{
				Node.Transform.Apply({Bounds.X, Bounds.Y}),
				Node.Transform.Apply({Bounds.X + Bounds.Width, Bounds.Y}),
				Node.Transform.Apply({Bounds.X + Bounds.Width, Bounds.Y + Bounds.Height}),
				Node.Transform.Apply({Bounds.X, Bounds.Y + Bounds.Height}),
			};
			const std::array<Vector2, 4> TextureCoordinates{
				Vector2(Uv.X, Uv.Y), Vector2(Uv.X + Uv.Width, Uv.Y),
				Vector2(Uv.X + Uv.Width, Uv.Y + Uv.Height), Vector2(Uv.X, Uv.Y + Uv.Height),
			};
			const auto Base = static_cast<std::uint32_t>(Batch->Vertices.size());
			if (Span) {
				Span->Batch = static_cast<std::size_t>(Batch - FrameValue.Batches.data());
				Span->FirstVertex = Base;
			}
			for (std::size_t Corner = 0; Corner < Logical.size(); ++Corner) {
				Batch->Vertices.push_back({
					{Logical[Corner].GetX() * Viewport.DpiScale, Logical[Corner].GetY() * Viewport.DpiScale},
					{TextureCoordinates[Corner].GetX(), TextureCoordinates[Corner].GetY()},
					{Color.R, Color.G, Color.B, ClampUnit(Alpha)},
				});
			}
			Batch->Indices.insert(Batch->Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 3, Base});
			PresentationVertices += 4;
			PresentationIndices += 6;
			++Profile.DisplayPrimitives;
			return true;
		}

		bool BuildPresentation() {
			const auto DisplayStart = Clock::now();
			auto Next = std::make_shared<GuiPresentationSnapshot>();
			std::unordered_map<ObjectId, PresentationSpan> NextSolidSpans;
			Next->Generation = NextPresentationGeneration++;
			Next->Frame.ViewportWidth = Viewport.PhysicalWidth;
			Next->Frame.ViewportHeight = Viewport.PhysicalHeight;
			Next->Frame.DpiScale = Viewport.DpiScale;
			Profile.DisplayPrimitives = 0;
			PresentationVertices = 0;
			PresentationIndices = 0;
			for (const auto &Node : Layout->Nodes) {
				if (!Node.EffectiveVisible || Node.EffectiveOpacity <= 0.0f || Node.Object == Node.Root) continue;
				const auto &Resolved = CommittedPresentation(Node);
				if (Resolved.BackgroundAlpha > 0.0f &&
					!AddQuad(Next->Frame, Node, Node.Bounds, {0, 0, 1, 1}, Resolved.BackgroundColor,
						Resolved.BackgroundAlpha, std::nullopt, Node.EffectiveClip,
						Resolved.Kind == GuiPresentationKind::Rectangle ? &NextSolidSpans[Node.Object] : nullptr)) return false;
				if (Resolved.Kind == GuiPresentationKind::Image && Resolved.ImageTexture &&
					!AddQuad(Next->Frame, Node, Node.Bounds, {0, 0, 1, 1}, Resolved.ContentColor,
						Resolved.ContentAlpha, Resolved.ImageTexture, Node.EffectiveClip)) return false;
				if (Resolved.SelectionWidth > 0.0f) {
					const GuiRect Selection{
						Node.Bounds.X + Resolved.TextOffsetX + Resolved.SelectionX,
						Node.Bounds.Y + Resolved.TextOffsetY,
						Resolved.SelectionWidth,
						Resolved.Text ? Resolved.Text->Height : Node.Bounds.Height,
					};
					if (!AddQuad(Next->Frame, Node, Selection, {0, 0, 1, 1}, Color3(0.20f, 0.48f, 0.90f),
						0.55f, std::nullopt, Node.EffectiveClip)) return false;
				}
				if (Resolved.Text && Resolved.ContentAlpha > 0.0f) {
					auto TextClip = Node.EffectiveClip ? GuiRect::Intersect(*Node.EffectiveClip, Node.TransformedBounds) :
						std::optional(Node.TransformedBounds);
					for (const auto &Glyph : Resolved.Text->Glyphs) {
						const GuiRect Destination{
							Node.Bounds.X + Resolved.TextOffsetX + Glyph.Destination.X,
							Node.Bounds.Y + Resolved.TextOffsetY + Glyph.Destination.Y,
							Glyph.Destination.Width, Glyph.Destination.Height,
						};
						if (!AddQuad(Next->Frame, Node, Destination, Glyph.Uv, Resolved.ContentColor,
							Resolved.ContentAlpha, Glyph.Texture, TextClip)) return false;
					}
				}
				if (Resolved.DrawCaret) {
					const GuiRect Caret{
						Node.Bounds.X + Resolved.TextOffsetX + Resolved.CaretX,
						Node.Bounds.Y + Resolved.TextOffsetY,
						std::max(1.0f / Viewport.DpiScale, 1.0f),
						Resolved.Text ? std::max(1.0f, Resolved.Text->Height) : Node.Bounds.Height,
					};
					if (!AddQuad(Next->Frame, Node, Caret, {0, 0, 1, 1}, Resolved.ContentColor,
						Resolved.ContentAlpha, std::nullopt, Node.EffectiveClip)) return false;
				}
			}
			for (const auto &Node : Layout->Nodes) {
				if (!Node.EffectiveVisible || !Node.ScrollContainer || Node.EffectiveOpacity <= 0.0f) continue;
				auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(ObjectRegistry::Get().Lookup(Node.Object));
				if (!Scroll || Scroll->GetScrollBarThickness() <= 0.0f) continue;
				const float MaximumY = std::max(0.0f, Node.ContentExtent.GetY() - Node.Bounds.Height);
				if (MaximumY <= 0.0f) continue;
				const float Thickness = std::min(Scroll->GetScrollBarThickness(), Node.Bounds.Width);
				const float ThumbHeight = std::max(12.0f, Node.Bounds.Height * Node.Bounds.Height / Node.ContentExtent.GetY());
				const float Travel = std::max(0.0f, Node.Bounds.Height - ThumbHeight);
				const GuiRect Thumb{
					Node.Bounds.X + Node.Bounds.Width - Thickness,
					Node.Bounds.Y + Travel * (Node.ScrollOffset.GetY() / MaximumY),
					Thickness, ThumbHeight,
				};
				if (!AddQuad(Next->Frame, Node, Thumb, {0, 0, 1, 1}, Color3(0.72f, 0.75f, 0.80f),
					0.72f, std::nullopt, Node.EffectiveClip)) return false;
			}
			const auto FrameConstruction = Nanoseconds(Clock::now() - DisplayStart);
			Profile.DisplayListNanoseconds += FrameConstruction;
			Profile.FrameConstructionNanoseconds += FrameConstruction;
			Profile.BatchCount = Next->Frame.Batches.size();
			SolidPresentationSpans = std::move(NextSolidSpans);
			Presentation = std::shared_ptr<const GuiPresentationSnapshot>(std::move(Next));
			return true;
		}

		bool PatchSolidPresentation() {
			if (!Presentation || PendingVisualObjects.empty()) return false;
			for (const auto Object : PendingVisualObjects) {
				const auto Span = SolidPresentationSpans.find(Object);
				const auto NodeIndex = Layout->NodeByObject.find(Object);
				if (Span == SolidPresentationSpans.end() || NodeIndex == Layout->NodeByObject.end()) return false;
				const auto &Node = Layout->Nodes[NodeIndex->second];
				const auto &Resolved = CommittedPresentation(Node);
				if (!Node.EffectiveVisible || Node.EffectiveOpacity <= 0.0f ||
					Resolved.Kind != GuiPresentationKind::Rectangle || Resolved.BackgroundAlpha <= 0.0f ||
					Span->second.Batch >= Presentation->Frame.Batches.size()) return false;
				const auto &Batch = Presentation->Frame.Batches[Span->second.Batch];
				std::optional<RenderUiClipRect> Clip;
				if (Node.EffectiveClip) Clip = RenderUiClipRect{
					Node.EffectiveClip->X * Viewport.DpiScale, Node.EffectiveClip->Y * Viewport.DpiScale,
					Node.EffectiveClip->Width * Viewport.DpiScale, Node.EffectiveClip->Height * Viewport.DpiScale,
				};
				const auto Layer = static_cast<std::int32_t>(std::clamp<std::int64_t>(
					Node.EffectiveLayer, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
				if (Batch.Texture || !SameClip(Batch.Clip, Clip) || Batch.Layer != Layer ||
					Batch.Opacity != Node.EffectiveOpacity || Span->second.FirstVertex > Batch.Vertices.size() ||
					Batch.Vertices.size() - Span->second.FirstVertex < 4) return false;
			}

			const auto Start = Clock::now();
			auto Next = std::make_shared<GuiPresentationSnapshot>();
			Next->Generation = NextPresentationGeneration++;
			Next->Frame = Presentation->Frame;
			for (const auto Object : PendingVisualObjects) {
				const auto &Span = SolidPresentationSpans.at(Object);
				const auto &Node = Layout->Nodes[Layout->NodeByObject.at(Object)];
				const auto &Resolved = CommittedPresentation(Node);
				const std::array<Vector2, 4> Logical{
					Node.Transform.Apply({Node.Bounds.X, Node.Bounds.Y}),
					Node.Transform.Apply({Node.Bounds.X + Node.Bounds.Width, Node.Bounds.Y}),
					Node.Transform.Apply({Node.Bounds.X + Node.Bounds.Width, Node.Bounds.Y + Node.Bounds.Height}),
					Node.Transform.Apply({Node.Bounds.X, Node.Bounds.Y + Node.Bounds.Height}),
				};
				const std::array<glm::vec2, 4> Uv{
					glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f),
				};
				auto &Vertices = Next->Frame.Batches[Span.Batch].Vertices;
				for (std::size_t Corner = 0; Corner < Logical.size(); ++Corner)
					Vertices[Span.FirstVertex + Corner] = {
						{Logical[Corner].GetX() * Viewport.DpiScale, Logical[Corner].GetY() * Viewport.DpiScale},
						Uv[Corner],
						{Resolved.BackgroundColor.R, Resolved.BackgroundColor.G, Resolved.BackgroundColor.B,
							ClampUnit(Resolved.BackgroundAlpha)},
					};
			}
			const auto Elapsed = Nanoseconds(Clock::now() - Start);
			Profile.DisplayPrimitives = PendingVisualObjects.size();
			Profile.BatchCount = Next->Frame.Batches.size();
			Profile.DisplayListNanoseconds += Elapsed;
			Profile.FrameConstructionNanoseconds += Elapsed;
			Presentation = std::shared_ptr<const GuiPresentationSnapshot>(std::move(Next));
			return true;
		}

		Enums::AccessibilityRole ResolveRole(const std::shared_ptr<GuiBase2d> &Object) const {
			if (Object->GetAccessibilityRole() != Enums::AccessibilityRole::Automatic) return Object->GetAccessibilityRole();
			if (std::dynamic_pointer_cast<ScreenGui>(Object)) return Enums::AccessibilityRole::Group;
			if (std::dynamic_pointer_cast<TextBox>(Object)) return Enums::AccessibilityRole::TextBox;
			if (std::dynamic_pointer_cast<ScrollingFrame>(Object)) return Enums::AccessibilityRole::ScrollView;
			if (std::dynamic_pointer_cast<TextButton>(Object)) return Enums::AccessibilityRole::Button;
			if (std::dynamic_pointer_cast<TextLabel>(Object)) return Enums::AccessibilityRole::Text;
			if (std::dynamic_pointer_cast<ImageLabel>(Object) && !Object->GetAccessibleName().empty()) return Enums::AccessibilityRole::Image;
			if (!Object->GetAccessibleName().empty()) return Enums::AccessibilityRole::Group;
			return Enums::AccessibilityRole::None;
		}

		void BuildAccessibility() {
			const auto Start = Clock::now();
			auto Next = std::make_shared<GuiAccessibilitySnapshot>();
			Next->Generation = NextAccessibilityGeneration++;
			std::unordered_set<ObjectId> Included;
			std::uint32_t Order = 0;
			std::vector<const GuiLayoutNode *> LogicalOrder;
			LogicalOrder.reserve(Layout->Nodes.size());
			for (const auto &Root : Roots) {
				auto Cache = RootCaches.find(Root->GetObjectId());
				if (Cache == RootCaches.end()) continue;
				for (const auto &Node : Cache->second.Nodes) LogicalOrder.push_back(&Node);
			}
			for (const auto *NodePointer : LogicalOrder) {
				const auto &Node = *NodePointer;
				if (!Node.EffectiveVisible) continue;
				auto Object = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Node.Object));
				if (!Object) continue;
				const auto Role = ResolveRole(Object);
				if (Role == Enums::AccessibilityRole::None) continue;
				ObjectId Parent = Node.Parent;
				while (Parent.IsValid() && !Included.contains(Parent)) {
					auto ParentNode = Layout->NodeByObject.find(Parent);
					if (ParentNode == Layout->NodeByObject.end()) { Parent = {}; break; }
					Parent = Layout->Nodes[ParentNode->second].Parent;
				}
				std::string Name = Object->GetAccessibleName();
				std::string Value;
				if (auto Input = std::dynamic_pointer_cast<TextBox>(Object)) {
					if (Name.empty()) Name = Input->GetPlaceholderText();
					Value = Input->GetSecureTextEntry() ? MaskEditableText(Input->GetText()) : Input->GetText();
				} else if (auto Label = std::dynamic_pointer_cast<TextLabel>(Object)) {
					if (Name.empty()) Name = Label->GetText();
					if (Role == Enums::AccessibilityRole::Text) Value = Label->GetText();
				}
				const bool Focused = FocusedByRoot.contains(Node.Root) && FocusedByRoot.at(Node.Root) == Node.Object;
				const auto GuiObjectValue = std::dynamic_pointer_cast<GuiObject>(Object);
				const auto ButtonValue = std::dynamic_pointer_cast<TextButton>(Object);
				const auto InputValue = std::dynamic_pointer_cast<TextBox>(Object);
				const auto ScrollValue = std::dynamic_pointer_cast<ScrollingFrame>(Object);
				const auto Editing = InputValue ? TextEditing.find(Node.Object) : TextEditing.end();
				Next->Nodes.push_back({
					Node.Object, Parent, Role, std::move(Name), std::move(Value), Order++,
					!ButtonValue || ButtonValue->GetInteractable(), Focused,
					GuiObjectValue && GuiObjectValue->GetGuiState() == Enums::GuiState::Press,
					Object->GetAccessibilitySelected(),
					InputValue != nullptr, InputValue && InputValue->GetReadOnly(),
					Editing != TextEditing.end() ? static_cast<std::uint32_t>(Editing->second.Caret) : 0,
					Editing != TextEditing.end() ? static_cast<std::uint32_t>(std::min(Editing->second.Caret, Editing->second.Anchor)) : 0,
					Editing != TextEditing.end() ? static_cast<std::uint32_t>(
						std::max(Editing->second.Caret, Editing->second.Anchor) - std::min(Editing->second.Caret, Editing->second.Anchor)) : 0,
					ScrollValue ? ScrollValue->GetCanvasPosition().GetX() : 0.0f,
					ScrollValue ? ScrollValue->GetCanvasPosition().GetY() : 0.0f,
					ScrollValue ? std::max(0.0f, ScrollValue->GetContentExtent().GetX() - Node.Bounds.Width) : 0.0f,
					ScrollValue ? std::max(0.0f, ScrollValue->GetContentExtent().GetY() - Node.Bounds.Height) : 0.0f,
				});
				Next->NodeByObject.emplace(Node.Object, Next->Nodes.size() - 1);
				Included.insert(Node.Object);
			}
			Accessibility = std::shared_ptr<const GuiAccessibilitySnapshot>(std::move(Next));
			Profile.AccessibilityNodes += Accessibility->Nodes.size();
			Profile.AccessibilityNanoseconds += Nanoseconds(Clock::now() - Start);
		}

		bool RefreshAccessibility(const std::unordered_map<ObjectId, std::uint32_t> &DirtyObjects) {
			const auto Start = Clock::now();
			auto Next = std::make_shared<GuiAccessibilitySnapshot>(*Accessibility);
			Next->Generation = NextAccessibilityGeneration++;
			for (const auto &[ObjectIdValue, Domains] : DirtyObjects) {
				if ((Domains & DirtyAccessibility) == 0) continue;
				const auto LayoutNode = Layout->NodeByObject.find(ObjectIdValue);
				const auto Existing = Next->NodeByObject.find(ObjectIdValue);
				if (LayoutNode == Layout->NodeByObject.end()) return false;
				const auto &Node = Layout->Nodes[LayoutNode->second];
				auto Object = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(ObjectIdValue));
				if (!Object) return false;
				const auto Role = ResolveRole(Object);
				if (!Node.EffectiveVisible || Role == Enums::AccessibilityRole::None || Existing == Next->NodeByObject.end())
					return false;
				auto &Target = Next->Nodes[Existing->second];
				Target.Role = Role;
				Target.Name = Object->GetAccessibleName();
				Target.Value.clear();
				if (auto Input = std::dynamic_pointer_cast<TextBox>(Object)) {
					if (Target.Name.empty()) Target.Name = Input->GetPlaceholderText();
					Target.Value = Input->GetSecureTextEntry() ? MaskEditableText(Input->GetText()) : Input->GetText();
					Target.Editable = true;
					Target.ReadOnly = Input->GetReadOnly();
					if (auto Editing = TextEditing.find(ObjectIdValue); Editing != TextEditing.end()) {
						Target.Caret = static_cast<std::uint32_t>(Editing->second.Caret);
						Target.SelectionStart = static_cast<std::uint32_t>(std::min(Editing->second.Caret, Editing->second.Anchor));
						Target.SelectionLength = static_cast<std::uint32_t>(
							std::max(Editing->second.Caret, Editing->second.Anchor) - std::min(Editing->second.Caret, Editing->second.Anchor));
					}
				} else if (auto Label = std::dynamic_pointer_cast<TextLabel>(Object)) {
					if (Target.Name.empty()) Target.Name = Label->GetText();
					if (Role == Enums::AccessibilityRole::Text) Target.Value = Label->GetText();
				}
				if (auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(Object)) {
					Target.ScrollPositionX = Scroll->GetCanvasPosition().GetX();
					Target.ScrollPositionY = Scroll->GetCanvasPosition().GetY();
					Target.ScrollMaximumX = std::max(0.0f, Scroll->GetContentExtent().GetX() - Node.Bounds.Width);
					Target.ScrollMaximumY = std::max(0.0f, Scroll->GetContentExtent().GetY() - Node.Bounds.Height);
				}
				Target.Enabled = !std::dynamic_pointer_cast<GuiObject>(Object) ||
					std::dynamic_pointer_cast<GuiObject>(Object)->GetInteractable();
				Target.Focused = FocusedByRoot.contains(Node.Root) && FocusedByRoot.at(Node.Root) == ObjectIdValue;
				Target.Pressed = std::dynamic_pointer_cast<GuiObject>(Object) &&
					std::dynamic_pointer_cast<GuiObject>(Object)->GetGuiState() == Enums::GuiState::Press;
				Target.Selected = Object->GetAccessibilitySelected();
				++Profile.AccessibilityNodes;
			}
			Accessibility = std::shared_ptr<const GuiAccessibilitySnapshot>(std::move(Next));
			Profile.AccessibilityNanoseconds += Nanoseconds(Clock::now() - Start);
			return true;
		}

		std::optional<ObjectId> HitTest(Vector2 LogicalPoint) {
			const auto Start = Clock::now();
			for (auto It = Layout->Nodes.rbegin(); It != Layout->Nodes.rend(); ++It) {
				const auto &Node = *It;
				if (!Node.EffectiveVisible || (!Node.Interactable && Node.InputSink == Enums::InputSink::None) ||
					(Node.EffectiveClip && !Node.EffectiveClip->Contains(LogicalPoint.GetX(), LogicalPoint.GetY()))) continue;
				auto Inverse = Node.Transform.Inverse();
				if (!Inverse) continue;
				const auto Local = Inverse->Apply(LogicalPoint);
				if (Node.Bounds.Contains(Local.GetX(), Local.GetY())) {
					Profile.HitTestNanoseconds += Nanoseconds(Clock::now() - Start);
					return Node.Object;
				}
			}
			Profile.HitTestNanoseconds += Nanoseconds(Clock::now() - Start);
			return std::nullopt;
		}

		std::vector<ObjectId> RouteFor(ObjectId Target) const {
			std::vector<ObjectId> Route;
			for (ObjectId Current = Target; Current.IsValid();) {
				Route.push_back(Current);
				if (Route.size() >= GuiLimits::MaximumPointerRouteDepth) break;
				auto Node = Layout->NodeByObject.find(Current);
				if (Node == Layout->NodeByObject.end()) break;
				Current = Layout->Nodes[Node->second].Parent;
			}
			std::ranges::reverse(Route);
			return Route;
		}

		enum class RoutedSignal { Move, Down, Up };

		bool Dispatch(ObjectId Target, const GuiPointerInput &Input, RoutedSignal SignalKind) {
			auto TargetObject = ObjectRegistry::Get().Lookup(Target);
			if (!TargetObject) return false;
			auto Event = std::make_shared<GuiInputEvent>();
			Event->Initialize(TargetObject, Input.PointerId,
				Input.PhysicalPosition / Viewport.DpiScale, Input.Type, Input.Button);
			const auto Route = RouteFor(Target);
			auto Fire = [&](ObjectId Current, Enums::GuiEventPhase Phase) {
				auto Object = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Current));
				if (!Object || Object->GetDataModel() != World) return;
				Event->SetRouteState(Object, Phase);
				switch (SignalKind) {
				case RoutedSignal::Move: Object->PointerMoved->Fire(Event); break;
				case RoutedSignal::Down: Object->PointerDown->Fire(Event); break;
				case RoutedSignal::Up: Object->PointerUp->Fire(Event); break;
				}
			};
			for (std::size_t Index = 0; Index + 1 < Route.size() && !Event->GetHandled(); ++Index)
				Fire(Route[Index], Enums::GuiEventPhase::Capture);
			if (!Route.empty() && !Event->GetHandled()) Fire(Route.back(), Enums::GuiEventPhase::Target);
			for (std::size_t Index = Route.size(); Index > 1 && !Event->GetHandled(); --Index)
				Fire(Route[Index - 2], Enums::GuiEventPhase::Bubble);
			return Event->GetHandled();
		}

		void FireDirect(ObjectId Target, const GuiPointerInput &Input, bool Enter) {
			auto Object = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Target));
			if (!Object) return;
			auto Event = std::make_shared<GuiInputEvent>();
			Event->Initialize(Object, Input.PointerId, Input.PhysicalPosition / Viewport.DpiScale, Input.Type, Input.Button);
			Event->SetRouteState(Object, Enums::GuiEventPhase::Target);
			if (Enter) Object->PointerEnter->Fire(Event);
			else Object->PointerLeave->Fire(Event);
		}

		void RefreshInteraction(ObjectId Object) {
			const auto Start = Clock::now();
			auto Gui = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Object));
			if (!Gui) return;
			const bool WasPressed = Gui->GetActive();
			const auto PreviousState = Gui->GetGuiState();
			const bool IsPressed = Gui->GetInteractable() &&
				std::ranges::any_of(Pressed, [&](const auto &Entry) { return Entry.second == Object; });
			const bool IsHovered = std::ranges::any_of(Hovered, [&](const auto &Entry) { return Entry.second == Object; });
			const auto StateValue = !Gui->GetInteractable() ? Enums::GuiState::NonInteractable :
				IsPressed ? Enums::GuiState::Press : IsHovered ? Enums::GuiState::Hover : Enums::GuiState::Idle;
			if (WasPressed == IsPressed && PreviousState == StateValue) return;
			Gui->CommitRuntimeInteraction(IsPressed, StateValue);
			Mark(Object, DirtyPresentation | (WasPressed != IsPressed ? DirtyAccessibility : DirtyNone));
			Profile.InteractionNanoseconds += Nanoseconds(Clock::now() - Start);
		}

		void SetHover(const GuiPointerInput &Input, std::optional<ObjectId> Hit) {
			const ObjectId Previous = Hovered.contains(Input.PointerId) ? Hovered.at(Input.PointerId) : ObjectId{};
			const ObjectId Next = Hit.value_or(ObjectId{});
			if (Previous == Next) return;
			if (Previous.IsValid()) {
				FireDirect(Previous, Input, false);
				Hovered.erase(Input.PointerId);
				RefreshInteraction(Previous);
			}
			if (Next.IsValid()) {
				Hovered[Input.PointerId] = Next;
				FireDirect(Next, Input, true);
				RefreshInteraction(Next);
			}
		}

		ObjectId RootForObject(ObjectId Object) const {
			auto Node = Layout->NodeByObject.find(Object);
			return Node == Layout->NodeByObject.end() ? ObjectId{} : Layout->Nodes[Node->second].Root;
		}

		void Focus(ObjectId Object) {
			auto Node = Layout->NodeByObject.find(Object);
			if (Node == Layout->NodeByObject.end() || !Layout->Nodes[Node->second].FocusEligible) return;
			const auto Root = Layout->Nodes[Node->second].Root;
			const ObjectId Previous = FocusedByRoot.contains(Root) ? FocusedByRoot.at(Root) : ObjectId{};
			if (Previous == Object) return;
			FocusedByRoot[Root] = Object;
			if (auto PreviousInput = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(Previous))) {
				if (auto Editing = TextEditing.find(Previous); Editing != TextEditing.end()) Editing->second.Composition.clear();
				PreviousInput->CommitRuntimeEditing(
					PreviousInput->GetCaretPosition(), PreviousInput->GetSelectionStart(), PreviousInput->GetSelectionLength(), "");
			}
			if (auto PreviousObject = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Previous)))
				PreviousObject->FocusLost->Fire({});
			if (auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(Object))) {
				auto &Editing = TextEditing[Object];
				const auto Count = Utf8Boundaries(Input->GetText()).size() - 1;
				Editing.Caret = std::min(Editing.Caret, Count);
				Editing.Anchor = std::min(Editing.Anchor, Count);
				Input->CommitRuntimeEditing(
					static_cast<int>(Editing.Caret), static_cast<int>(std::min(Editing.Caret, Editing.Anchor)),
					static_cast<int>(std::max(Editing.Caret, Editing.Anchor) - std::min(Editing.Caret, Editing.Anchor)),
					Editing.Composition);
			}
			if (auto NextObject = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Object)))
				NextObject->Focused->Fire({});
			TextInputCommandDirty = true;
			Mark(Object, DirtyAccessibility | DirtyPresentation);
			if (Previous.IsValid()) Mark(Previous, DirtyAccessibility | DirtyPresentation);
		}

		void FocusNext() {
			ObjectId ActiveRoot;
			for (auto It = Roots.rbegin(); It != Roots.rend(); ++It) {
				const auto Root = (*It)->GetObjectId();
				if (FocusedByRoot.contains(Root)) { ActiveRoot = Root; break; }
			}
			if (!ActiveRoot.IsValid()) {
				for (auto It = Roots.rbegin(); It != Roots.rend(); ++It) {
					const auto Root = (*It)->GetObjectId();
					const bool HasEligible = std::ranges::any_of(Layout->Nodes, [Root](const GuiLayoutNode &Node) {
						return Node.Root == Root && Node.EffectiveVisible && Node.FocusEligible;
					});
					if (HasEligible) { ActiveRoot = Root; break; }
				}
			}
			std::vector<const GuiLayoutNode *> Eligible;
			if (auto Cache = RootCaches.find(ActiveRoot); Cache != RootCaches.end())
				for (const auto &Node : Cache->second.Nodes)
					if (Node.EffectiveVisible && Node.FocusEligible) Eligible.push_back(&Node);
			if (Eligible.empty()) return;
			const ObjectId Current = FocusedByRoot.contains(ActiveRoot) ? FocusedByRoot.at(ActiveRoot) : ObjectId{};
			auto It = std::ranges::find(Eligible, Current, [](const GuiLayoutNode *Node) { return Node->Object; });
			if (It == Eligible.end() || ++It == Eligible.end()) Focus(Eligible.front()->Object);
			else Focus((*It)->Object);
		}

		ObjectId KeyboardFocus() const {
			for (auto It = Roots.rbegin(); It != Roots.rend(); ++It) {
				const auto Existing = FocusedByRoot.find((*It)->GetObjectId());
				if (Existing != FocusedByRoot.end() && Existing->second.IsValid()) return Existing->second;
			}
			return {};
		}

		void CommitEditingState(const std::shared_ptr<TextBox> &Input, TextEditingState &Editing) {
			if (!Input) return;
			const auto Count = Utf8Boundaries(Input->GetText()).size() - 1;
			Editing.Caret = std::min(Editing.Caret, Count);
			Editing.Anchor = std::min(Editing.Anchor, Count);
			Input->CommitRuntimeEditing(
				static_cast<int>(Editing.Caret), static_cast<int>(std::min(Editing.Caret, Editing.Anchor)),
				static_cast<int>(std::max(Editing.Caret, Editing.Anchor) - std::min(Editing.Caret, Editing.Anchor)),
				Editing.Composition);
			Mark(Input->GetObjectId(), DirtyPresentation | DirtyAccessibility);
			TextInputCommandDirty = true;
		}

		bool ReplaceTextSelection(const std::shared_ptr<TextBox> &Input, std::string_view Insert) {
			if (!Input || Input->GetReadOnly() || Insert.size() > GuiLimits::MaximumEditableTextBytes ||
				TextEditsThisFrame >= GuiLimits::MaximumTextInputEditsPerFrame) return false;
			++TextEditsThisFrame;
			auto &Editing = TextEditing[Input->GetObjectId()];
			const auto NormalizedExisting = NormalizeEditableUtf8(Input->GetText());
			const auto &Existing = NormalizedExisting.Bytes;
			const auto ExistingBoundaries = Utf8Boundaries(Existing);
			const auto NormalizedInsert = NormalizeEditableUtf8(Insert);
			const auto InsertBoundaries = Utf8Boundaries(NormalizedInsert.Bytes);
			const std::size_t Start = std::min({Editing.Caret, Editing.Anchor, ExistingBoundaries.size() - 1});
			const std::size_t End = std::min(std::max(Editing.Caret, Editing.Anchor), ExistingBoundaries.size() - 1);
			const std::size_t MaximumCodePoints = std::min<std::size_t>(
				std::max(0, Input->GetMaxLength()), GuiLimits::MaximumEditableCodePoints);
			const std::size_t RetainedCount = ExistingBoundaries.size() - 1 - (End - Start);
			const std::size_t InsertCount = std::min(InsertBoundaries.size() - 1,
				RetainedCount < MaximumCodePoints ? MaximumCodePoints - RetainedCount : 0);
			const auto InsertBytes = std::string_view(NormalizedInsert.Bytes).substr(0, InsertBoundaries[InsertCount]);
			std::string Next;
			Next.reserve(std::min(GuiLimits::MaximumEditableTextBytes,
				Existing.size() - (ExistingBoundaries[End] - ExistingBoundaries[Start]) + InsertBytes.size()));
			Next.append(Existing, 0, ExistingBoundaries[Start]);
			Next.append(InsertBytes);
			Next.append(Existing, ExistingBoundaries[End], std::string::npos);
			if (Next.size() > GuiLimits::MaximumEditableTextBytes) return false;
			Input->SetText(std::move(Next));
			Editing.Caret = Start + InsertCount;
			Editing.Anchor = Editing.Caret;
			Editing.Composition.clear();
			CommitEditingState(Input, Editing);
			return true;
		}

		bool HandleTextKey(const KeyEvent &Key) {
			if (Key.State != ButtonState::Pressed) return false;
			auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(KeyboardFocus()));
			if (!Input || !Input->GetInteractable()) return false;
			auto &Editing = TextEditing[Input->GetObjectId()];
			const auto Boundaries = Utf8Boundaries(Input->GetText());
			const auto Count = Boundaries.size() - 1;
			Editing.Caret = std::min(Editing.Caret, Count);
			Editing.Anchor = std::min(Editing.Anchor, Count);
			const bool Extend = HasModifier(Key.Modifiers, KeyModifier::Shift);
			auto Move = [&](std::size_t Position) {
				Editing.Caret = std::min(Position, Count);
				if (!Extend) Editing.Anchor = Editing.Caret;
				CommitEditingState(Input, Editing);
			};
			switch (Key.Logical) {
			case LogicalKey::Left: Move(Editing.Caret > 0 ? Editing.Caret - 1 : 0); return true;
			case LogicalKey::Right: Move(std::min(Count, Editing.Caret + 1)); return true;
			case LogicalKey::Home: Move(0); return true;
			case LogicalKey::End: Move(Count); return true;
			case LogicalKey::Backspace:
				if (Input->GetReadOnly()) return true;
				if (Editing.Caret == Editing.Anchor && Editing.Caret > 0) Editing.Anchor = Editing.Caret - 1;
				return ReplaceTextSelection(Input, {});
			case LogicalKey::Delete:
				if (Input->GetReadOnly()) return true;
				if (Editing.Caret == Editing.Anchor && Editing.Caret < Count) Editing.Anchor = Editing.Caret + 1;
				return ReplaceTextSelection(Input, {});
			case LogicalKey::Return:
				if (Input->GetMultiLine()) return ReplaceTextSelection(Input, "\n");
				Input->Submitted->Fire(Input->GetText());
				return true;
			default: return false;
			}
		}

		void PlaceTextCaret(ObjectId Object, Vector2 LogicalPosition, bool Extend) {
			if (SelectionOperationsThisFrame >= GuiLimits::MaximumSelectionOperationsPerFrame) return;
			++SelectionOperationsThisFrame;
			auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(Object));
			if (!Input) return;
			auto NodeIndex = Layout->NodeByObject.find(Object);
			if (NodeIndex == Layout->NodeByObject.end()) return;
			const auto &Node = Layout->Nodes[NodeIndex->second];
			auto Inverse = Node.Transform.Inverse();
			const auto &PresentationValue = CommittedPresentation(Node);
			if (!Inverse || !PresentationValue.Text || PresentationValue.Text->CaretOffsets.empty()) return;
			const auto Local = Inverse->Apply(LogicalPosition);
			const float X = Local.GetX() - Node.Bounds.X - PresentationValue.TextOffsetX;
			const auto &Offsets = PresentationValue.Text->CaretOffsets;
			std::size_t Closest = 0;
			float Distance = std::abs(Offsets.front() - X);
			for (std::size_t Index = 1; Index < Offsets.size(); ++Index) {
				const float Candidate = std::abs(Offsets[Index] - X);
				if (Candidate < Distance) { Closest = Index; Distance = Candidate; }
			}
			auto &Editing = TextEditing[Object];
			Editing.Caret = Closest;
			if (!Extend) Editing.Anchor = Closest;
			CommitEditingState(Input, Editing);
		}

		ObjectId NearestScroll(ObjectId Object) const {
			for (auto Current = Object; Current.IsValid();) {
				if (std::dynamic_pointer_cast<ScrollingFrame>(ObjectRegistry::Get().Lookup(Current))) return Current;
				auto Node = Layout->NodeByObject.find(Current);
				if (Node == Layout->NodeByObject.end()) break;
				Current = Layout->Nodes[Node->second].Parent;
			}
			return {};
		}

		bool ScrollBy(ObjectId Start, Vector2 Delta) {
			for (ObjectId Current = NearestScroll(Start); Current.IsValid();) {
				auto Scroll = std::dynamic_pointer_cast<ScrollingFrame>(ObjectRegistry::Get().Lookup(Current));
				auto NodeIndex = Layout->NodeByObject.find(Current);
				if (!Scroll || NodeIndex == Layout->NodeByObject.end()) return false;
				const auto &Node = Layout->Nodes[NodeIndex->second];
				const auto Direction = Scroll->GetScrollingDirection();
				const auto Position = Scroll->GetCanvasPosition();
				const float MaximumX = std::max(0.0f, Scroll->GetContentExtent().GetX() - Node.Bounds.Width);
				const float MaximumY = std::max(0.0f, Scroll->GetContentExtent().GetY() - Node.Bounds.Height);
				const float X = Direction == Enums::ScrollingDirection::Y ? Position.GetX() :
					std::clamp(Position.GetX() + Delta.GetX(), 0.0f, MaximumX);
				const float Y = Direction == Enums::ScrollingDirection::X ? Position.GetY() :
					std::clamp(Position.GetY() + Delta.GetY(), 0.0f, MaximumY);
				if (X != Position.GetX() || Y != Position.GetY()) {
					Scroll->SetCanvasPosition({X, Y});
					return true;
				}
				const auto Parent = Node.Parent;
				Current = NearestScroll(Parent);
			}
			return false;
		}

		bool HandleScrollKey(const KeyEvent &Key) {
			if (Key.State != ButtonState::Pressed) return false;
			const auto Focused = KeyboardFocus();
			const auto Scroll = NearestScroll(Focused);
			if (!Scroll.IsValid()) return false;
			auto Node = Layout->NodeByObject.find(Scroll);
			if (Node == Layout->NodeByObject.end()) return false;
			const float Page = std::max(24.0f, Layout->Nodes[Node->second].Bounds.Height * 0.9f);
			switch (Key.Logical) {
			case LogicalKey::Up: return ScrollBy(Scroll, {0.0f, -40.0f});
			case LogicalKey::Down: return ScrollBy(Scroll, {0.0f, 40.0f});
			case LogicalKey::PageUp: return ScrollBy(Scroll, {0.0f, -Page});
			case LogicalKey::PageDown: return ScrollBy(Scroll, {0.0f, Page});
			case LogicalKey::Home: return ScrollBy(Scroll, {0.0f, -GuiLimits::MaximumScrollExtent});
			case LogicalKey::End: return ScrollBy(Scroll, {0.0f, GuiLimits::MaximumScrollExtent});
			default: return false;
			}
		}

		void CleanupTransient() {
			auto IsInvalid = [&](ObjectId Object) {
				return !Object.IsValid() || !ObjectRegistry::Get().Lookup(Object) || !Layout->NodeByObject.contains(Object);
			};
			std::erase_if(Hovered, [&](const auto &Entry) { return IsInvalid(Entry.second); });
			std::erase_if(Pressed, [&](const auto &Entry) { return IsInvalid(Entry.second); });
			std::erase_if(Captured, [&](const auto &Entry) { return IsInvalid(Entry.second); });
			std::erase_if(ScrollGestures, [&](const auto &Entry) {
				return IsInvalid(Entry.second.Scroll) || IsInvalid(Entry.second.InitialTarget);
			});
			const auto FocusCount = FocusedByRoot.size();
			std::erase_if(FocusedByRoot, [&](const auto &Entry) { return IsInvalid(Entry.first) || IsInvalid(Entry.second); });
			if (FocusedByRoot.size() != FocusCount) TextInputCommandDirty = true;
			std::erase_if(TextEditing, [&](const auto &Entry) { return IsInvalid(Entry.first); });
			if (IsInvalid(KeyboardPressed)) KeyboardPressed = {};
		}
	};

	GuiRuntime::GuiRuntime(std::shared_ptr<DataModel> World, std::filesystem::path DefaultFontPath)
		: State(std::make_unique<Impl>(std::move(World), std::move(DefaultFontPath))) {
		std::scoped_lock Lock(RuntimeRegistryMutex);
		RuntimeRegistry.emplace(State->World.get(), this);
	}

	GuiRuntime::~GuiRuntime() {
		std::scoped_lock Lock(RuntimeRegistryMutex);
		RuntimeRegistry.erase(State->World.get());
	}

	GuiRuntime *GuiRuntime::Find(const Instance &Object) {
		auto World = Object.GetDataModel();
		if (!World) return nullptr;
		std::scoped_lock Lock(RuntimeRegistryMutex);
		auto Existing = RuntimeRegistry.find(World.get());
		return Existing == RuntimeRegistry.end() ? nullptr : Existing->second;
	}

	void GuiRuntime::SetViewport(GuiViewportConfiguration Configuration) {
		if (!Configuration.IsValid()) throw std::invalid_argument("GUI viewport configuration is invalid");
		if (State->Viewport == Configuration) return;
		State->Viewport = Configuration;
		State->TextInputCommandDirty = true;
		for (const auto &[Root, Cache] : State->RootCaches) {
			(void)Cache;
			State->RootDirty[Root] |= DirtyAll;
		}
		State->PresentationDirty = true;
	}

	bool GuiRuntime::Reconcile() {
		const auto DirtyStart = Clock::now();
		State->Profile = {};
		State->Profile.ObservationNanoseconds = std::exchange(State->PendingObservationNanoseconds, 0);
		State->Profile.DirtyMarkingNanoseconds = std::exchange(State->PendingDirtyMarkingNanoseconds, 0);
		State->LayoutPassesThisFrame = 0;
		State->ShapedGlyphsThisFrame = 0;
		State->TextEditsThisFrame = 0;
		State->SelectionOperationsThisFrame = 0;
		State->MeasuredTextThisFrame.clear();
		State->PendingVisualObjects.clear();
		auto AssetChanges = State->Assets->ReadChanges(State->AssetChangeSequence);
		State->AssetChangeSequence = AssetChanges.NextSequence;
		if (AssetChanges.RescanRequired || !AssetChanges.Changes.empty()) {
			for (const auto &[Object, Connections] : State->Observers) {
				(void)Connections;
				auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
				if (auto Image = std::dynamic_pointer_cast<ImageLabel>(InstanceValue)) {
					const bool Affected = AssetChanges.RescanRequired || std::ranges::any_of(AssetChanges.Changes, [&](const AssetChange &Change) {
						return Change.Kind == AssetKind::Image && Change.Reference == Image->GetImage();
					});
					if (Affected) State->Mark(Object, DirtyLayout | DirtyPresentation | DirtyAccessibility);
				}
				if (auto Text = std::dynamic_pointer_cast<TextLabel>(InstanceValue)) {
					const bool Affected = AssetChanges.RescanRequired || std::ranges::any_of(AssetChanges.Changes, [&](const AssetChange &Change) {
						return Change.Kind == AssetKind::Font && Change.Reference == Text->GetFontFace();
					});
					if (Affected) State->Mark(Object, DirtyLayout | DirtyText | DirtyPresentation | DirtyAccessibility);
				}
			}
		}
		State->Text.ResetFrameBudget();
		bool StructureChanged = false;
		if (State->StructureDirty) {
			State->Roots = State->DiscoverRoots();
			std::unordered_set<ObjectId> Active;
			for (const auto &Root : State->Roots) {
				Active.insert(Root->GetObjectId());
				State->RootDirty[Root->GetObjectId()] |= DirtyAll;
			}
			std::erase_if(State->RootCaches, [&](const auto &Entry) { return !Active.contains(Entry.first); });
			State->StructureDirty = false;
			State->PresentationDirty = true;
			StructureChanged = true;
		}
		const auto &Roots = State->Roots;
		auto DirtyObjects = std::move(State->ObjectDirty);
		State->ObjectDirty.clear();
		State->DirtyEpochs.clear();
		State->Profile.DirtyObjects = DirtyObjects.size();
		State->Profile.SemanticDirtyNanoseconds += Nanoseconds(Clock::now() - DirtyStart);

		bool SnapshotChanged = StructureChanged;
		bool VisualChanged = StructureChanged;
		bool FullPresentationRequired = StructureChanged;
		bool AccessibilityChanged = false;
		bool ForceAccessibilityRebuild = StructureChanged;
		std::unordered_map<ObjectId, std::uint32_t> FailedDirty;
		for (const auto &Root : Roots) {
			const auto Id = Root->GetObjectId();
			const std::uint32_t Domains = State->RootDirty.contains(Id) ? State->RootDirty[Id] : DirtyNone;
			if (Domains == DirtyNone) continue;
			bool Rebuilt = false;
			if (!State->RootCaches.contains(Id) || (Domains & DirtyText) != 0) {
				auto Built = State->BuildRoot(Root);
				if (Built) {
					State->RootCaches[Id] = std::move(*Built);
					SnapshotChanged = true;
					VisualChanged = true;
					Rebuilt = true;
					FullPresentationRequired = true;
					ForceAccessibilityRebuild = ForceAccessibilityRebuild || DirtyObjects.empty();
				} else FailedDirty[Id] |= Domains;
			} else if ((Domains & DirtyLayout) != 0) {
				auto Refreshed = State->RefreshLayout(State->RootCaches.at(Id), DirtyObjects);
				if (!Refreshed) {
					auto Built = State->BuildRoot(Root);
					if (!Built) { FailedDirty[Id] |= Domains; continue; }
					State->RootCaches[Id] = std::move(*Built);
					SnapshotChanged = true;
					VisualChanged = true;
					Rebuilt = true;
					FullPresentationRequired = true;
				} else {
					SnapshotChanged = SnapshotChanged || Refreshed->SnapshotChanged;
					VisualChanged = VisualChanged || Refreshed->VisualChanged;
					AccessibilityChanged = AccessibilityChanged || Refreshed->AccessibilityChanged;
				}
			}
			if (!Rebuilt && !FailedDirty.contains(Id) && (Domains & DirtyScroll) != 0) {
				auto Refreshed = State->RefreshScroll(State->RootCaches.at(Id), DirtyObjects);
				SnapshotChanged = SnapshotChanged || Refreshed.SnapshotChanged;
				VisualChanged = VisualChanged || Refreshed.VisualChanged;
				AccessibilityChanged = AccessibilityChanged || Refreshed.AccessibilityChanged;
			}
			if (!Rebuilt && !FailedDirty.contains(Id)) {
				auto Refreshed = State->RefreshRoot(State->RootCaches.at(Id), DirtyObjects);
				SnapshotChanged = SnapshotChanged || Refreshed.SnapshotChanged;
				VisualChanged = VisualChanged || Refreshed.VisualChanged;
				AccessibilityChanged = AccessibilityChanged || Refreshed.AccessibilityChanged;
			}
			AccessibilityChanged = AccessibilityChanged || (Domains & DirtyAccessibility) != 0;
		}
		State->RootDirty = std::move(FailedDirty);
		if (!State->RootDirty.empty()) {
			for (const auto &[Object, Domains] : DirtyObjects) {
				auto Root = State->FindRoot(ObjectRegistry::Get().Lookup(Object));
				if (Root && State->RootDirty.contains(Root->GetObjectId())) State->ObjectDirty[Object] |= Domains;
			}
		}
		if (SnapshotChanged) {
			const auto SnapshotStart = Clock::now();
			State->MergeSnapshots(Roots);
			State->Profile.SnapshotCommitNanoseconds += Nanoseconds(Clock::now() - SnapshotStart);
			if (std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(State->KeyboardFocus())))
				State->TextInputCommandDirty = true;
		}
		State->CleanupTransient();
		if (ForceAccessibilityRebuild) State->BuildAccessibility();
		else if (AccessibilityChanged && !State->RefreshAccessibility(DirtyObjects)) State->BuildAccessibility();
		State->PresentationDirty = State->PresentationDirty || VisualChanged;
		if (State->PresentationDirty) {
			const bool Presented = (!FullPresentationRequired && State->PatchSolidPresentation()) || State->BuildPresentation();
			if (!Presented) {
				State->AddDiagnostic("DisplayLimit", "GUI display list exceeded a Foundation 1 resource bound; the previous presentation was retained", {});
			} else {
				State->PresentationDirty = false;
			}
		}

		auto TextChanges = State->Text.DrainTextureChanges();
		auto ImageChanges = State->Assets->DrainTextureChanges();
		State->PendingTextures.Creates.insert(State->PendingTextures.Creates.end(),
			std::make_move_iterator(TextChanges.Creates.begin()), std::make_move_iterator(TextChanges.Creates.end()));
		State->PendingTextures.Creates.insert(State->PendingTextures.Creates.end(),
			std::make_move_iterator(ImageChanges.Creates.begin()), std::make_move_iterator(ImageChanges.Creates.end()));
		State->PendingTextures.Updates.insert(State->PendingTextures.Updates.end(),
			std::make_move_iterator(TextChanges.Updates.begin()), std::make_move_iterator(TextChanges.Updates.end()));
		State->PendingTextures.Updates.insert(State->PendingTextures.Updates.end(),
			std::make_move_iterator(ImageChanges.Updates.begin()), std::make_move_iterator(ImageChanges.Updates.end()));
		State->PendingTextures.Removes.insert(State->PendingTextures.Removes.end(),
			std::make_move_iterator(TextChanges.Removes.begin()), std::make_move_iterator(TextChanges.Removes.end()));
		State->PendingTextures.Removes.insert(State->PendingTextures.Removes.end(),
			std::make_move_iterator(ImageChanges.Removes.begin()), std::make_move_iterator(ImageChanges.Removes.end()));
		State->PendingTextures.UploadBytes += TextChanges.UploadBytes + ImageChanges.UploadBytes;
		if (State->PendingTextures.UploadBytes > GuiLimits::MaximumTextureUploadBytesPerFrame)
			State->AddDiagnostic("TextureUploadLimit", "GUI texture uploads exceeded the per-frame bound", {});
		auto TextProfile = State->Text.ConsumeProfile();
		State->Profile.TextShapingNanoseconds += TextProfile.TextShapingNanoseconds;
		State->Profile.GlyphLookupNanoseconds += TextProfile.GlyphLookupNanoseconds;
		State->Profile.GlyphRasterizationNanoseconds += TextProfile.GlyphRasterizationNanoseconds;
		State->Profile.AtlasUpdateNanoseconds += TextProfile.AtlasUpdateNanoseconds;
		State->Profile.ShapedGlyphs += TextProfile.ShapedGlyphs;
		State->Profile.TextureUpdates += TextProfile.TextureUpdates + ImageChanges.Creates.size() + ImageChanges.Updates.size();
		State->Profile.TextureUploadBytes += TextProfile.TextureUploadBytes + ImageChanges.UploadBytes;
		return SnapshotChanged || VisualChanged || !State->PendingTextures.Creates.empty() ||
			!State->PendingTextures.Updates.empty() || !State->PendingTextures.Removes.empty();
	}

	void GuiRuntime::Publish(RenderPublisher &Publisher) {
		const auto Start = Clock::now();
		const auto Source = State->World->GetObjectId();
		if (!Publisher.HasUiFrame(Source, State->Presentation->Generation)) {
			const auto CopyStart = Clock::now();
			Publisher.SetUiFrame(
				std::shared_ptr<const RenderUiFrame>(State->Presentation, &State->Presentation->Frame),
				Source,
				State->Presentation->Generation
			);
			State->Profile.FrameCopyNanoseconds += Nanoseconds(Clock::now() - CopyStart);
		}
		if (!State->PendingTextures.Creates.empty() || !State->PendingTextures.Updates.empty() ||
			!State->PendingTextures.Removes.empty()) {
			Publisher.SetUiTextureChanges(
				std::move(State->PendingTextures.Creates), std::move(State->PendingTextures.Updates),
				std::move(State->PendingTextures.Removes)
			);
			State->PendingTextures = {};
		}
		State->Profile.PublicationNanoseconds += Nanoseconds(Clock::now() - Start);
	}

	bool GuiRuntime::ProcessEvent(const HostEvent &Event) {
		if (const auto *Move = std::get_if<PointerMoveEvent>(&Event)) {
			return ProcessPointer({static_cast<int>(Move->Device.Value), Enums::GuiPointerType::Mouse,
				Enums::GuiPointerButton::None, GuiPointerAction::Move, {Move->Position.X, Move->Position.Y}});
		}
		if (const auto *Button = std::get_if<PointerButtonEvent>(&Event)) {
			return ProcessPointer({static_cast<int>(Button->Device.Value), Enums::GuiPointerType::Mouse,
				ConvertButton(Button->Button), Button->State == ButtonState::Pressed ? GuiPointerAction::Down : GuiPointerAction::Up,
				{Button->Position.X, Button->Position.Y}});
		}
		if (const auto *Touch = std::get_if<TouchPointerEvent>(&Event)) {
			const auto Action = Touch->Action == TouchPointerAction::Down ? GuiPointerAction::Down :
				Touch->Action == TouchPointerAction::Move ? GuiPointerAction::Move :
				Touch->Action == TouchPointerAction::Up ? GuiPointerAction::Up : GuiPointerAction::Cancel;
			return ProcessPointer({static_cast<int>(Touch->Pointer.Value), Enums::GuiPointerType::Touch,
				Enums::GuiPointerButton::Primary, Action, {Touch->Position.X, Touch->Position.Y}});
		}
		if (const auto *Wheel = std::get_if<WheelEvent>(&Event)) {
			const Vector2 Logical{Wheel->Position.X / State->Viewport.DpiScale, Wheel->Position.Y / State->Viewport.DpiScale};
			const auto Hit = State->HitTest(Logical);
			return Hit && State->ScrollBy(*Hit, {
				-Wheel->Delta.X * 40.0f / State->Viewport.DpiScale,
				-Wheel->Delta.Y * 40.0f / State->Viewport.DpiScale,
			});
		}
		if (const auto *Text = std::get_if<TextInputEvent>(&Event)) {
			auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(State->KeyboardFocus()));
			return Input && Input->GetInteractable() && State->ReplaceTextSelection(Input, Text->Text.View());
		}
		if (const auto *Composition = std::get_if<TextEditingEvent>(&Event)) {
			auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(State->KeyboardFocus()));
			if (!Input || !Input->GetInteractable()) return false;
			auto &Editing = State->TextEditing[Input->GetObjectId()];
			Editing.Composition.assign(Composition->Text.View());
			Editing.CompositionSelectionStart = static_cast<std::size_t>(std::max(0, Composition->SelectionStart));
			Editing.CompositionSelectionLength = static_cast<std::size_t>(std::max(0, Composition->SelectionLength));
			State->CommitEditingState(Input, Editing);
			return true;
		}
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && !Focus->Focused) {
			ClearTransientState();
			return false;
		}
		if (const auto *Key = std::get_if<KeyEvent>(&Event)) {
			if (Key->Logical == LogicalKey::Tab && Key->State == ButtonState::Pressed) {
				State->FocusNext();
				return true;
			}
			if (State->HandleTextKey(*Key)) return true;
			if (State->HandleScrollKey(*Key)) return true;
			if (Key->Logical != LogicalKey::Space && Key->Logical != LogicalKey::Return) return false;
			const ObjectId Focused = State->KeyboardFocus();
			auto Button = std::dynamic_pointer_cast<TextButton>(ObjectRegistry::Get().Lookup(Focused));
			if (!Button) return false;
			if (!Button->GetInteractable()) {
				if (State->KeyboardPressed == Focused) {
					State->KeyboardPressed = {};
					Button->CommitRuntimeInteraction(false, Enums::GuiState::NonInteractable);
					State->Mark(Focused, DirtyPresentation | DirtyAccessibility);
				}
				return false;
			}
			if (Key->State == ButtonState::Pressed) {
				State->KeyboardPressed = Focused;
				Button->CommitRuntimeInteraction(true, Enums::GuiState::Press);
				State->Mark(Focused, DirtyPresentation | DirtyAccessibility);
			} else if (State->KeyboardPressed == Focused) {
				State->KeyboardPressed = {};
				Button->CommitRuntimeInteraction(false, Enums::GuiState::Idle);
				Button->Activated->Fire({});
				State->Mark(Focused, DirtyPresentation | DirtyAccessibility);
			}
			return true;
		}
		return false;
	}

	bool GuiRuntime::ProcessPointer(const GuiPointerInput &Input) {
		if (Input.PointerId < 0 || !std::isfinite(Input.PhysicalPosition.GetX()) ||
			!std::isfinite(Input.PhysicalPosition.GetY())) return false;
		const Vector2 Logical = Input.PhysicalPosition / State->Viewport.DpiScale;
		const auto Hit = State->HitTest(Logical);
		if (Input.Action == GuiPointerAction::Move && Input.Type != Enums::GuiPointerType::Touch) State->SetHover(Input, Hit);
		if (Input.Type == Enums::GuiPointerType::Touch) {
			if (Input.Action == GuiPointerAction::Down && Hit) {
				const auto Scroll = State->NearestScroll(*Hit);
				if (Scroll.IsValid()) State->ScrollGestures[Input.PointerId] = {Scroll, *Hit, Input.PhysicalPosition, Input.PhysicalPosition, false};
			} else if (Input.Action == GuiPointerAction::Move) {
				if (auto Gesture = State->ScrollGestures.find(Input.PointerId); Gesture != State->ScrollGestures.end()) {
					auto &Value = Gesture->second;
					const auto FromStart = Input.PhysicalPosition - Value.StartPhysical;
					if (!Value.Dragging && (std::abs(FromStart.GetX()) >= 6.0f || std::abs(FromStart.GetY()) >= 6.0f)) {
						Value.Dragging = true;
						State->Captured[Input.PointerId] = Value.Scroll;
						const auto Pressed = State->Pressed.contains(Input.PointerId) ? State->Pressed.at(Input.PointerId) : ObjectId{};
						State->Pressed.erase(Input.PointerId);
						State->RefreshInteraction(Pressed);
					}
					if (Value.Dragging) {
						const auto Delta = (Value.LastPhysical - Input.PhysicalPosition) / State->Viewport.DpiScale;
						Value.LastPhysical = Input.PhysicalPosition;
						(void)State->ScrollBy(Value.Scroll, Delta);
						return true;
					}
				}
			} else if (Input.Action == GuiPointerAction::Up || Input.Action == GuiPointerAction::Cancel) {
				if (auto Gesture = State->ScrollGestures.find(Input.PointerId); Gesture != State->ScrollGestures.end()) {
					const bool Dragging = Gesture->second.Dragging;
					State->ScrollGestures.erase(Gesture);
					if (Dragging) {
						State->Captured.erase(Input.PointerId);
						State->Pressed.erase(Input.PointerId);
						return true;
					}
				}
			}
		}
		ObjectId Target;
		if (auto Captured = State->Captured.find(Input.PointerId); Captured != State->Captured.end() &&
			ObjectRegistry::Get().Lookup(Captured->second)) Target = Captured->second;
		else if (Hit) Target = *Hit;
		if (!Target.IsValid()) {
			if (Input.Action == GuiPointerAction::Up || Input.Action == GuiPointerAction::Cancel) {
				const ObjectId Pressed = State->Pressed.contains(Input.PointerId) ? State->Pressed.at(Input.PointerId) : ObjectId{};
				State->Pressed.erase(Input.PointerId);
				State->Captured.erase(Input.PointerId);
				State->RefreshInteraction(Pressed);
			}
			State->CleanupTransient();
			return false;
		}
		const auto Node = State->Layout->NodeByObject.find(Target);
		const auto Sink = Node == State->Layout->NodeByObject.end() ? Enums::InputSink::None :
			State->Layout->Nodes[Node->second].InputSink;
		const auto TargetObject = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Target));
		const bool Interactable = TargetObject && TargetObject->GetInteractable();
		bool Handled = false;
		if (Input.Action == GuiPointerAction::Move) Handled = State->Dispatch(Target, Input, Impl::RoutedSignal::Move);
		else if (Input.Action == GuiPointerAction::Down) {
			if (Interactable) {
				if (Node != State->Layout->NodeByObject.end() && State->Layout->Nodes[Node->second].FocusEligible) State->Focus(Target);
				State->PlaceTextCaret(Target, Logical, false);
				if (State->Captured.size() < GuiLimits::MaximumCapturedPointers) State->Captured[Input.PointerId] = Target;
				State->Pressed[Input.PointerId] = Target;
				State->RefreshInteraction(Target);
			}
			Handled = State->Dispatch(Target, Input, Impl::RoutedSignal::Down);
		} else if (Input.Action == GuiPointerAction::Up || Input.Action == GuiPointerAction::Cancel) {
			Handled = State->Dispatch(Target, Input, Impl::RoutedSignal::Up);
			const ObjectId Pressed = State->Pressed.contains(Input.PointerId) ? State->Pressed.at(Input.PointerId) : ObjectId{};
			State->Pressed.erase(Input.PointerId);
			State->Captured.erase(Input.PointerId);
			State->RefreshInteraction(Pressed);
			if (Input.Action == GuiPointerAction::Up && Hit && *Hit == Pressed) {
				if (auto Button = std::dynamic_pointer_cast<TextButton>(ObjectRegistry::Get().Lookup(Pressed));
					Button && Button->GetInteractable())
					Button->Activated->Fire({});
			}
		}
		if (Input.Action == GuiPointerAction::Move && State->Pressed.contains(Input.PointerId))
			State->PlaceTextCaret(State->Pressed.at(Input.PointerId), Logical, true);
		State->CleanupTransient();
		return Handled || Sink == Enums::InputSink::All ||
			(Sink == Enums::InputSink::Activate && Input.Action != GuiPointerAction::Move);
	}

	std::optional<HostCommand> GuiRuntime::SynchronizeTextInput() {
		if (!State->TextInputCommandDirty) return std::nullopt;
		State->TextInputCommandDirty = false;
		const auto Focused = State->KeyboardFocus();
		auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(Focused));
		auto Node = State->Layout->NodeByObject.find(Focused);
		if (!Input || Node == State->Layout->NodeByObject.end() || !State->Layout->Nodes[Node->second].FocusEligible) {
			if (!State->TextInputWasActive) return std::nullopt;
			State->TextInputWasActive = false;
			return HostCommand{SetTextInputState{}};
		}
		const auto &LayoutNode = State->Layout->Nodes[Node->second];
		const auto &Resolved = State->CommittedPresentation(LayoutNode);
		State->TextInputWasActive = true;
		return HostCommand{SetTextInputState{
			.Active = true,
			.Secure = Input->GetSecureTextEntry(),
			.Multiline = Input->GetMultiLine(),
			.AutocorrectEnabled = !Input->GetSecureTextEntry(),
			.X = static_cast<std::int32_t>(std::lround(LayoutNode.Bounds.X * State->Viewport.DpiScale)),
			.Y = static_cast<std::int32_t>(std::lround(LayoutNode.Bounds.Y * State->Viewport.DpiScale)),
			.Width = static_cast<std::int32_t>(std::lround(LayoutNode.Bounds.Width * State->Viewport.DpiScale)),
			.Height = static_cast<std::int32_t>(std::lround(LayoutNode.Bounds.Height * State->Viewport.DpiScale)),
			.Cursor = static_cast<std::int32_t>(std::lround(
				(LayoutNode.Bounds.X + Resolved.TextOffsetX + Resolved.CaretX) * State->Viewport.DpiScale)),
		}};
	}

	void GuiRuntime::RequestFocus(ObjectId Object) { State->Focus(Object); }
	void GuiRuntime::ReleaseFocus(ObjectId Object) {
		const auto Root = State->RootForObject(Object);
		if (!Root.IsValid() || !State->FocusedByRoot.contains(Root) || State->FocusedByRoot.at(Root) != Object) return;
		State->FocusedByRoot.erase(Root);
		if (auto Gui = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Object))) Gui->FocusLost->Fire({});
		if (auto Editing = State->TextEditing.find(Object); Editing != State->TextEditing.end()) {
			Editing->second.Composition.clear();
			if (auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(Object)))
				Input->CommitRuntimeEditing(
					static_cast<int>(Editing->second.Caret),
					static_cast<int>(std::min(Editing->second.Caret, Editing->second.Anchor)),
					static_cast<int>(std::max(Editing->second.Caret, Editing->second.Anchor) -
						std::min(Editing->second.Caret, Editing->second.Anchor)),
					"");
		}
		State->TextInputCommandDirty = true;
		State->Mark(Object, DirtyAccessibility | DirtyPresentation);
	}

	void GuiRuntime::CapturePointer(int PointerId, ObjectId Object) {
		if (PointerId < 0 || !State->Layout->NodeByObject.contains(Object)) return;
		if (!State->Captured.contains(PointerId) && State->Captured.size() >= GuiLimits::MaximumCapturedPointers) return;
		State->Captured[PointerId] = Object;
	}

	void GuiRuntime::ReleasePointer(int PointerId, ObjectId Object) {
		if (auto Existing = State->Captured.find(PointerId); Existing != State->Captured.end() && Existing->second == Object)
			State->Captured.erase(Existing);
	}

	void GuiRuntime::ClearTransientState() {
		std::unordered_set<ObjectId> Changed;
		for (const auto &[Pointer, Object] : State->Hovered) { (void)Pointer; Changed.insert(Object); }
		for (const auto &[Pointer, Object] : State->Pressed) { (void)Pointer; Changed.insert(Object); }
		for (const auto &[Root, Object] : State->FocusedByRoot) {
			(void)Root;
			if (auto Gui = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Object))) Gui->FocusLost->Fire({});
			Changed.insert(Object);
		}
		for (auto &[Object, Editing] : State->TextEditing) {
			Editing.Composition.clear();
			if (auto Input = std::dynamic_pointer_cast<TextBox>(ObjectRegistry::Get().Lookup(Object)))
				Input->CommitRuntimeEditing(
					static_cast<int>(Editing.Caret), static_cast<int>(std::min(Editing.Caret, Editing.Anchor)),
					static_cast<int>(std::max(Editing.Caret, Editing.Anchor) - std::min(Editing.Caret, Editing.Anchor)), "");
		}
		State->Hovered.clear();
		State->Pressed.clear();
		State->Captured.clear();
		State->ScrollGestures.clear();
		State->FocusedByRoot.clear();
		State->TextInputCommandDirty = true;
		State->KeyboardPressed = {};
		for (const auto Object : Changed) State->RefreshInteraction(Object);
	}

	std::string GuiRuntime::RegisterImage(
		std::string LogicalId,
		std::uint32_t Width,
		std::uint32_t Height,
		std::span<const std::uint8_t> Rgba8
	) {
		auto Reference = State->Assets->RegisterMemoryImage(std::move(LogicalId), Width, Height, Rgba8);
		for (const auto &[Root, Cache] : State->RootCaches) {
			(void)Cache;
			State->RootDirty[Root] |= DirtyLayout | DirtyPresentation;
		}
		State->PresentationDirty = true;
		return Reference;
	}

	std::shared_ptr<const GuiLayoutSnapshot> GuiRuntime::GetCommittedLayout() const { return State->Layout; }
	std::shared_ptr<const GuiPresentationSnapshot> GuiRuntime::GetCommittedPresentation() const { return State->Presentation; }
	std::shared_ptr<const GuiAccessibilitySnapshot> GuiRuntime::GetAccessibilitySnapshot() const { return State->Accessibility; }
	GuiRuntimeProfile GuiRuntime::GetLastProfile() const { return State->Profile; }
	std::vector<GuiDiagnostic> GuiRuntime::GetDiagnostics() const { return {State->Diagnostics.begin(), State->Diagnostics.end()}; }
}
