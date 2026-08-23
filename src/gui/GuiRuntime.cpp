// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/gui/GuiRuntime.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/LayerCollector.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
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
			DirtyAll = DirtyLayout | DirtyPresentation | DirtyText | DirtyInput | DirtyAccessibility,
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

		Enums::GuiPointerButton ConvertButton(PointerButton Button) {
			switch (Button) {
			case PointerButton::Left: return Enums::GuiPointerButton::Primary;
			case PointerButton::Right: return Enums::GuiPointerButton::Secondary;
			case PointerButton::Middle: return Enums::GuiPointerButton::Middle;
			default: return Enums::GuiPointerButton::None;
			}
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
			std::optional<GuiImageResource> Image;
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
			std::vector<GuiLayoutNode> Nodes;
			bool AutomaticFallback = false;
		};

		std::shared_ptr<DataModel> World;
		GuiViewportConfiguration Viewport;
		GuiTextSystem Text;
		GuiImageStore Images;
		std::function<void()> DescendantBinding;
		SignalConnection::Pointer DescendantRemovedConnection;
		std::unordered_map<ObjectId, std::vector<SignalConnection::Pointer>> Observers;
		std::unordered_map<ObjectId, RootCache> RootCaches;
		std::vector<std::shared_ptr<ScreenGui>> Roots;
		std::unordered_map<ObjectId, std::uint32_t> RootDirty;
		std::unordered_map<int, ObjectId> Hovered;
		std::unordered_map<int, ObjectId> Pressed;
		std::unordered_map<int, ObjectId> Captured;
		std::unordered_map<ObjectId, ObjectId> FocusedByRoot;
		ObjectId KeyboardPressed;
		std::shared_ptr<const GuiLayoutSnapshot> Layout = std::make_shared<const GuiLayoutSnapshot>();
		std::shared_ptr<const GuiPresentationSnapshot> Presentation = std::make_shared<const GuiPresentationSnapshot>();
		std::shared_ptr<const GuiAccessibilitySnapshot> Accessibility = std::make_shared<const GuiAccessibilitySnapshot>();
		GuiTextureChanges PendingTextures;
		GuiRuntimeProfile Profile;
		std::deque<GuiDiagnostic> Diagnostics;
		std::uint64_t NextDiagnosticSequence = 1;
		std::uint64_t NextLayoutGeneration = 1;
		std::uint64_t NextPresentationGeneration = 1;
		std::uint64_t NextAccessibilityGeneration = 1;
		std::size_t PresentationVertices = 0;
		std::size_t PresentationIndices = 0;
		std::size_t LayoutPassesThisFrame = 0;
		std::size_t ShapedGlyphsThisFrame = 0;
		std::unordered_set<ObjectId> MeasuredTextThisFrame;
		bool StructureDirty = true;
		bool PresentationDirty = true;

		Impl(std::shared_ptr<DataModel> WorldValue, std::filesystem::path FontPath)
			: World(std::move(WorldValue)), Text(std::move(FontPath)) {
			if (!World) throw std::invalid_argument("GuiRuntime requires a DataModel");
			Viewport = {1, 1, 1.0f, {}};
			DescendantBinding = World->BindDescendants([this](std::shared_ptr<Instance> Object) {
				Observe(Object);
				if (IsGuiRelevant(Object)) StructureDirty = true;
			});
			DescendantRemovedConnection = World->DescendantRemoved->Connect([this](std::shared_ptr<Instance> Object) {
				const auto Id = Object->GetObjectId();
				Observers.erase(Id);
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
			auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
			auto Root = FindRoot(InstanceValue);
			if (!Root) {
				StructureDirty = true;
				return;
			}
			RootDirty[Root->GetObjectId()] |= Domains;
			if ((Domains & DirtyPresentation) != 0) PresentationDirty = true;
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
			for (const auto *Name : {"Position", "Size", "AnchorPoint", "AutomaticSize", "Visible", "Opacity",
				"Rotation", "ZIndex", "LayoutOrder", "ClipsDescendants", "Enabled", "DisplayOrder", "ClipToSafeArea",
				"FillDirection", "Padding"}) Connect(Name, DirtyLayout | DirtyPresentation | DirtyInput | DirtyAccessibility);
			for (const auto *Name : {"BackgroundColor3", "BackgroundTransparency", "TextColor3", "TextTransparency",
				"ImageColor3", "ImageTransparency"}) Connect(Name, DirtyPresentation);
			for (const auto *Name : {"Text", "TextSize", "TextWrapped", "TextXAlignment", "TextYAlignment", "FontFace"})
				Connect(Name, DirtyLayout | DirtyText | DirtyPresentation | DirtyAccessibility);
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
			if (auto Button = std::dynamic_pointer_cast<TextButton>(Object)) {
				Result.Kind = GuiPresentationKind::Button;
				Result.ContentColor = Button->GetTextColor3();
				Result.ContentAlpha = 1.0f - ClampUnit(Button->GetTextTransparency());
				Result.Text = Working.Text;
				const float StateScale = Button->GetGuiState() == Enums::GuiState::Press ? 0.78f :
					Button->GetGuiState() == Enums::GuiState::Hover ? 1.08f : 1.0f;
				Result.BackgroundColor = Color3(
					std::clamp(Result.BackgroundColor.R * StateScale, 0.0f, 1.0f),
					std::clamp(Result.BackgroundColor.G * StateScale, 0.0f, 1.0f),
					std::clamp(Result.BackgroundColor.B * StateScale, 0.0f, 1.0f)
				);
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
			} else if (std::dynamic_pointer_cast<Frame>(Object)) {
				Result.Kind = GuiPresentationKind::Rectangle;
			}
			if (Result.Text) {
				auto Label = std::dynamic_pointer_cast<TextLabel>(Object);
				if (Label->GetTextXAlignment() == Enums::TextXAlignment::Center)
					Result.TextOffsetX = (Working.Bounds.Width - Result.Text->Width) * 0.5f;
				else if (Label->GetTextXAlignment() == Enums::TextXAlignment::Right)
					Result.TextOffsetX = Working.Bounds.Width - Result.Text->Width;
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
			}

			const auto MeasureStart = Clock::now();
			for (auto &Node : Working) {
				float DesiredWidth = Node.Bounds.Width;
				float DesiredHeight = Node.Bounds.Height;
				if (auto Label = std::dynamic_pointer_cast<TextLabel>(Node.Object)) {
					Node.Text = Text.Shape({
						Label->GetText(), Label->GetFontFace(), Label->GetTextSize(), Viewport.DpiScale,
						Node.Bounds.Width, Label->GetTextWrapped(), static_cast<int>(Label->GetTextXAlignment())
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
					Node.Image = Images.Find(Image->GetImage());
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
				ContentWidths[Child.Parent] = std::max(ContentWidths[Child.Parent],
					Child.Bounds.X + Child.Bounds.Width - Working[Child.Parent].Bounds.X);
				ContentHeights[Child.Parent] = std::max(ContentHeights[Child.Parent],
					Child.Bounds.Y + Child.Bounds.Height - Working[Child.Parent].Bounds.Y);
			}
			for (std::size_t Reverse = Working.size(); Reverse-- > 0;) {
				auto &Node = Working[Reverse];
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
			if (Root->GetZIndexBehavior() == Enums::ZIndexBehavior::Sibling)
				AddDiagnostic("ZIndexFallback", "Foundation 1 resolves Sibling ZIndexBehavior with deterministic global ordering", Root->GetObjectId());
			std::vector<WorkingNode> Working;
			if (!CollectWorking(Root, Working)) return std::nullopt;
			const auto Lists = BuildLists(Root, Working);
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
			Cache.AutomaticFallback = !Converged;
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
				Committed.Presentation = ResolvePresentation(Node);
				Cache.Nodes.push_back(std::move(Committed));
				Node.Object->CommitRuntimeGeometry(
					{Node.Bounds.X, Node.Bounds.Y}, {Node.Bounds.Width, Node.Bounds.Height}, Node.Object->GetRotation()
				);
			}
			++Profile.LayoutRoots;
			return Cache;
		}

		void RefreshRoot(RootCache &Cache, std::uint32_t Domains) {
			for (auto &Node : Cache.Nodes) {
				if (Node.Object == Cache.Root) continue;
				auto Object = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Node.Object));
				if (!Object) continue;
				if ((Domains & DirtyInput) != 0) {
					Node.Interactable = Object->GetInteractable();
					Node.FocusEligible = Object->GetInteractable() && Object->GetSelectable();
					Node.InputSink = Object->GetInputSink();
				}
				if ((Domains & DirtyPresentation) != 0) {
					WorkingNode Working;
					Working.Object = Object;
					Working.Bounds = Node.Bounds;
					Working.Text = Node.Presentation.Text;
					if (auto Image = std::dynamic_pointer_cast<ImageLabel>(Object)) Working.Image = Images.Find(Image->GetImage());
					Node.Presentation = ResolvePresentation(Working);
				}
			}
		}

		void MergeSnapshots(const std::vector<std::shared_ptr<ScreenGui>> &Roots) {
			auto Next = std::make_shared<GuiLayoutSnapshot>();
			Next->Generation = NextLayoutGeneration++;
			Next->Viewport = Viewport;
			for (const auto &Root : Roots) {
				auto Cache = RootCaches.find(Root->GetObjectId());
				if (Cache == RootCaches.end()) continue;
				Next->Nodes.insert(Next->Nodes.end(), Cache->second.Nodes.begin(), Cache->second.Nodes.end());
			}
			std::ranges::stable_sort(Next->Nodes, [](const GuiLayoutNode &Left, const GuiLayoutNode &Right) {
				return Left.EffectiveLayer != Right.EffectiveLayer ? Left.EffectiveLayer < Right.EffectiveLayer :
					Left.TreeOrder < Right.TreeOrder;
			});
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
			std::optional<GuiRect> LogicalClip
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
			Next->Generation = NextPresentationGeneration++;
			Next->Frame.ViewportWidth = Viewport.PhysicalWidth;
			Next->Frame.ViewportHeight = Viewport.PhysicalHeight;
			Next->Frame.DpiScale = Viewport.DpiScale;
			Profile.DisplayPrimitives = 0;
			PresentationVertices = 0;
			PresentationIndices = 0;
			for (const auto &Node : Layout->Nodes) {
				if (!Node.EffectiveVisible || Node.EffectiveOpacity <= 0.0f || Node.Object == Node.Root) continue;
				const auto &Resolved = Node.Presentation;
				if (Resolved.BackgroundAlpha > 0.0f &&
					!AddQuad(Next->Frame, Node, Node.Bounds, {0, 0, 1, 1}, Resolved.BackgroundColor,
						Resolved.BackgroundAlpha, std::nullopt, Node.EffectiveClip)) return false;
				if (Resolved.Kind == GuiPresentationKind::Image && Resolved.ImageTexture &&
					!AddQuad(Next->Frame, Node, Node.Bounds, {0, 0, 1, 1}, Resolved.ContentColor,
						Resolved.ContentAlpha, Resolved.ImageTexture, Node.EffectiveClip)) return false;
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
			}
			Profile.DisplayListNanoseconds += Nanoseconds(Clock::now() - DisplayStart);
			Profile.BatchCount = Next->Frame.Batches.size();
			Presentation = std::shared_ptr<const GuiPresentationSnapshot>(std::move(Next));
			return true;
		}

		Enums::AccessibilityRole ResolveRole(const std::shared_ptr<GuiBase2d> &Object) const {
			if (Object->GetAccessibilityRole() != Enums::AccessibilityRole::Automatic) return Object->GetAccessibilityRole();
			if (std::dynamic_pointer_cast<ScreenGui>(Object)) return Enums::AccessibilityRole::Group;
			if (std::dynamic_pointer_cast<TextButton>(Object)) return Enums::AccessibilityRole::Button;
			if (std::dynamic_pointer_cast<TextLabel>(Object)) return Enums::AccessibilityRole::Text;
			if (std::dynamic_pointer_cast<ImageLabel>(Object) && !Object->GetAccessibleName().empty()) return Enums::AccessibilityRole::Image;
			if (!Object->GetAccessibleName().empty()) return Enums::AccessibilityRole::Group;
			return Enums::AccessibilityRole::None;
		}

		void BuildAccessibility() {
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
				if (auto Label = std::dynamic_pointer_cast<TextLabel>(Object)) {
					if (Name.empty()) Name = Label->GetText();
					if (Role == Enums::AccessibilityRole::Text) Value = Label->GetText();
				}
				const bool Focused = FocusedByRoot.contains(Node.Root) && FocusedByRoot.at(Node.Root) == Node.Object;
				const auto GuiObjectValue = std::dynamic_pointer_cast<GuiObject>(Object);
				const auto ButtonValue = std::dynamic_pointer_cast<TextButton>(Object);
				Next->Nodes.push_back({
					Node.Object, Parent, Role, std::move(Name), std::move(Value), Order++,
					!ButtonValue || ButtonValue->GetInteractable(), Focused,
					GuiObjectValue && GuiObjectValue->GetGuiState() == Enums::GuiState::Press,
					Object->GetAccessibilitySelected(),
				});
				Included.insert(Node.Object);
			}
			Accessibility = std::shared_ptr<const GuiAccessibilitySnapshot>(std::move(Next));
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
			auto Gui = std::dynamic_pointer_cast<GuiObject>(ObjectRegistry::Get().Lookup(Object));
			if (!Gui) return;
			const bool IsPressed = std::ranges::any_of(Pressed, [&](const auto &Entry) { return Entry.second == Object; });
			const bool IsHovered = std::ranges::any_of(Hovered, [&](const auto &Entry) { return Entry.second == Object; });
			const auto StateValue = !Gui->GetInteractable() ? Enums::GuiState::NonInteractable :
				IsPressed ? Enums::GuiState::Press : IsHovered ? Enums::GuiState::Hover : Enums::GuiState::Idle;
			Gui->CommitRuntimeInteraction(IsPressed, StateValue);
			Mark(Object, DirtyPresentation | DirtyAccessibility);
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
			if (auto PreviousObject = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Previous)))
				PreviousObject->FocusLost->Fire({});
			if (auto NextObject = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Object)))
				NextObject->Focused->Fire({});
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

		void CleanupTransient() {
			auto IsInvalid = [&](ObjectId Object) {
				return !Object.IsValid() || !ObjectRegistry::Get().Lookup(Object) || !Layout->NodeByObject.contains(Object);
			};
			std::erase_if(Hovered, [&](const auto &Entry) { return IsInvalid(Entry.second); });
			std::erase_if(Pressed, [&](const auto &Entry) { return IsInvalid(Entry.second); });
			std::erase_if(Captured, [&](const auto &Entry) { return IsInvalid(Entry.second); });
			std::erase_if(FocusedByRoot, [&](const auto &Entry) { return IsInvalid(Entry.first) || IsInvalid(Entry.second); });
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
		for (const auto &[Root, Cache] : State->RootCaches) {
			(void)Cache;
			State->RootDirty[Root] |= DirtyAll;
		}
		State->PresentationDirty = true;
	}

	bool GuiRuntime::Reconcile() {
		const auto DirtyStart = Clock::now();
		State->Profile = {};
		State->LayoutPassesThisFrame = 0;
		State->ShapedGlyphsThisFrame = 0;
		State->MeasuredTextThisFrame.clear();
		State->Text.ResetFrameBudget(State->Images.PendingUploadBytes());
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
		State->Profile.SemanticDirtyNanoseconds += Nanoseconds(Clock::now() - DirtyStart);

		bool LayoutChanged = StructureChanged;
		bool AccessibilityChanged = false;
		std::unordered_map<ObjectId, std::uint32_t> FailedDirty;
		for (const auto &Root : Roots) {
			const auto Id = Root->GetObjectId();
			const std::uint32_t Domains = State->RootDirty.contains(Id) ? State->RootDirty[Id] : DirtyNone;
			if (Domains == DirtyNone) continue;
			if ((Domains & (DirtyLayout | DirtyText)) != 0 || !State->RootCaches.contains(Id)) {
				auto Built = State->BuildRoot(Root);
				if (Built) {
					State->RootCaches[Id] = std::move(*Built);
					LayoutChanged = true;
					State->PresentationDirty = true;
				} else FailedDirty[Id] |= Domains;
			} else {
				State->RefreshRoot(State->RootCaches.at(Id), Domains);
				LayoutChanged = true;
			}
			AccessibilityChanged = AccessibilityChanged || (Domains & DirtyAccessibility) != 0;
		}
		State->RootDirty = std::move(FailedDirty);
		if (LayoutChanged) State->MergeSnapshots(Roots);
		State->CleanupTransient();
		if (LayoutChanged || AccessibilityChanged) State->BuildAccessibility();
		if (State->PresentationDirty) {
			if (!State->BuildPresentation()) {
				State->AddDiagnostic("DisplayLimit", "GUI display list exceeded a Foundation 1 resource bound; the previous presentation was retained", {});
			} else {
				State->PresentationDirty = false;
			}
		}

		auto TextChanges = State->Text.DrainTextureChanges();
		auto ImageChanges = State->Images.DrainTextureChanges();
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
		return LayoutChanged || !State->PendingTextures.Creates.empty() ||
			!State->PendingTextures.Updates.empty() || !State->PendingTextures.Removes.empty();
	}

	void GuiRuntime::Publish(RenderPublisher &Publisher) {
		const auto Start = Clock::now();
		Publisher.SetUiFrame(State->Presentation->Frame);
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
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && !Focus->Focused) {
			ClearTransientState();
			return false;
		}
		if (const auto *Key = std::get_if<KeyEvent>(&Event)) {
			if (Key->Logical == LogicalKey::Tab && Key->State == ButtonState::Pressed) {
				State->FocusNext();
				return true;
			}
			if (Key->Logical != LogicalKey::Space && Key->Logical != LogicalKey::Return) return false;
			const ObjectId Focused = State->KeyboardFocus();
			auto Button = std::dynamic_pointer_cast<TextButton>(ObjectRegistry::Get().Lookup(Focused));
			if (!Button) return false;
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
		if (Input.Action == GuiPointerAction::Move) State->SetHover(Input, Hit);
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
		bool Handled = false;
		if (Input.Action == GuiPointerAction::Move) Handled = State->Dispatch(Target, Input, Impl::RoutedSignal::Move);
		else if (Input.Action == GuiPointerAction::Down) {
			if (Node != State->Layout->NodeByObject.end() && State->Layout->Nodes[Node->second].FocusEligible) State->Focus(Target);
			if (State->Captured.size() < GuiLimits::MaximumCapturedPointers) State->Captured[Input.PointerId] = Target;
			State->Pressed[Input.PointerId] = Target;
			State->RefreshInteraction(Target);
			Handled = State->Dispatch(Target, Input, Impl::RoutedSignal::Down);
		} else if (Input.Action == GuiPointerAction::Up || Input.Action == GuiPointerAction::Cancel) {
			Handled = State->Dispatch(Target, Input, Impl::RoutedSignal::Up);
			const ObjectId Pressed = State->Pressed.contains(Input.PointerId) ? State->Pressed.at(Input.PointerId) : ObjectId{};
			State->Pressed.erase(Input.PointerId);
			State->Captured.erase(Input.PointerId);
			State->RefreshInteraction(Pressed);
			if (Input.Action == GuiPointerAction::Up && Hit && *Hit == Pressed) {
				if (auto Button = std::dynamic_pointer_cast<TextButton>(ObjectRegistry::Get().Lookup(Pressed)))
					Button->Activated->Fire({});
			}
		}
		State->CleanupTransient();
		return Handled || Sink == Enums::InputSink::All ||
			(Sink == Enums::InputSink::Activate && Input.Action != GuiPointerAction::Move);
	}

	void GuiRuntime::RequestFocus(ObjectId Object) { State->Focus(Object); }
	void GuiRuntime::ReleaseFocus(ObjectId Object) {
		const auto Root = State->RootForObject(Object);
		if (!Root.IsValid() || !State->FocusedByRoot.contains(Root) || State->FocusedByRoot.at(Root) != Object) return;
		State->FocusedByRoot.erase(Root);
		if (auto Gui = std::dynamic_pointer_cast<GuiBase2d>(ObjectRegistry::Get().Lookup(Object))) Gui->FocusLost->Fire({});
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
		State->Hovered.clear();
		State->Pressed.clear();
		State->Captured.clear();
		State->FocusedByRoot.clear();
		State->KeyboardPressed = {};
		for (const auto Object : Changed) State->RefreshInteraction(Object);
	}

	void GuiRuntime::RegisterImage(
		std::string LogicalId,
		std::uint32_t Width,
		std::uint32_t Height,
		std::span<const std::uint8_t> Rgba8
	) {
		State->Images.Register(std::move(LogicalId), Width, Height, Rgba8);
		for (const auto &[Root, Cache] : State->RootCaches) {
			(void)Cache;
			State->RootDirty[Root] |= DirtyLayout | DirtyPresentation;
		}
		State->PresentationDirty = true;
	}

	std::shared_ptr<const GuiLayoutSnapshot> GuiRuntime::GetCommittedLayout() const { return State->Layout; }
	std::shared_ptr<const GuiPresentationSnapshot> GuiRuntime::GetCommittedPresentation() const { return State->Presentation; }
	std::shared_ptr<const GuiAccessibilitySnapshot> GuiRuntime::GetAccessibilitySnapshot() const { return State->Accessibility; }
	GuiRuntimeProfile GuiRuntime::GetLastProfile() const { return State->Profile; }
	std::vector<GuiDiagnostic> GuiRuntime::GetDiagnostics() const { return {State->Diagnostics.begin(), State->Diagnostics.end()}; }
}
