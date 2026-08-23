// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/classes/GuiBase2d.hpp"
#include "gargantuan/classes/GuiObject.hpp"
#include "gargantuan/render/RenderPublication.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	struct GuiRect {
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;

		[[nodiscard]] bool Contains(float PointX, float PointY) const;
		[[nodiscard]] bool IsEmpty() const { return Width <= 0.0f || Height <= 0.0f; }
		[[nodiscard]] static std::optional<GuiRect> Intersect(const GuiRect &Left, const GuiRect &Right);
	};

	struct GuiTransform {
		float M00 = 1.0f;
		float M01 = 0.0f;
		float M10 = 0.0f;
		float M11 = 1.0f;
		float Tx = 0.0f;
		float Ty = 0.0f;

		[[nodiscard]] Vector2 Apply(Vector2 Point) const;
		[[nodiscard]] std::optional<GuiTransform> Inverse() const;
		[[nodiscard]] GuiTransform Then(const GuiTransform &Child) const;
		[[nodiscard]] static GuiTransform RotationAbout(float Degrees, float CenterX, float CenterY);
	};

	struct GuiSafeAreaInsets {
		float Left = 0.0f;
		float Top = 0.0f;
		float Right = 0.0f;
		float Bottom = 0.0f;
		auto operator<=>(const GuiSafeAreaInsets &) const = default;
	};

	struct GuiViewportConfiguration {
		std::uint32_t PhysicalWidth = 1;
		std::uint32_t PhysicalHeight = 1;
		float DpiScale = 1.0f;
		GuiSafeAreaInsets SafeArea{};

		[[nodiscard]] float LogicalWidth() const { return static_cast<float>(PhysicalWidth) / DpiScale; }
		[[nodiscard]] float LogicalHeight() const { return static_cast<float>(PhysicalHeight) / DpiScale; }
		[[nodiscard]] bool IsValid() const;
		auto operator<=>(const GuiViewportConfiguration &) const = default;
	};

	enum class GuiPresentationKind : std::uint8_t { None, Rectangle, Text, Image, Button, ScrollView, TextInput };

	struct GuiTextGlyph {
		RenderTextureIdentity Texture;
		GuiRect Destination;
		GuiRect Uv;
	};

	struct GuiShapedText {
		float Width = 0.0f;
		float Height = 0.0f;
		std::vector<GuiTextGlyph> Glyphs;
		// Logical x coordinates at Unicode code-point boundaries. Editable text
		// uses SDL_ttf's shaped substring mapping rather than byte offsets.
		std::vector<float> CaretOffsets;
		std::size_t InputBytes = 0;
		bool ReplacedInvalidUtf8 = false;
		bool TruncatedInput = false;
		bool UsedMissingGlyph = false;
		bool ResourceLimit = false;
	};

	struct GuiResolvedPresentation {
		GuiPresentationKind Kind = GuiPresentationKind::None;
		Color3 BackgroundColor{1.0f, 1.0f, 1.0f};
		float BackgroundAlpha = 0.0f;
		Color3 ContentColor{1.0f, 1.0f, 1.0f};
		float ContentAlpha = 1.0f;
		std::optional<RenderTextureIdentity> ImageTexture;
		std::shared_ptr<const GuiShapedText> Text;
		float TextOffsetX = 0.0f;
		float TextOffsetY = 0.0f;
		bool Hovered = false;
		bool Pressed = false;
		bool Focused = false;
		bool Enabled = true;
		bool Selected = false;
		bool Editable = false;
		bool ReadOnly = false;
		bool DrawCaret = false;
		float CaretX = 0.0f;
		float SelectionX = 0.0f;
		float SelectionWidth = 0.0f;
	};

	struct GuiLayoutNode {
		ObjectId Object;
		ObjectId Parent;
		ObjectId Root;
		GuiRect Bounds;
		GuiRect TransformedBounds;
		GuiTransform Transform;
		std::optional<GuiRect> EffectiveClip;
		std::int64_t EffectiveLayer = 0;
		std::uint32_t TreeOrder = 0;
		std::uint32_t PaintOrder = 0;
		std::uint32_t Depth = 0;
		int LayoutOrder = 0;
		bool EffectiveVisible = false;
		float EffectiveOpacity = 1.0f;
		bool Interactable = false;
		bool FocusEligible = false;
		Enums::InputSink InputSink = Enums::InputSink::None;
		bool ScrollContainer = false;
		Vector2 ScrollOffset;
		Vector2 ContentExtent;
		GuiResolvedPresentation Presentation;
	};

	struct GuiDiagnostic {
		std::uint64_t Sequence = 0;
		std::string Code;
		std::string Message;
		ObjectId Object;
	};

	struct GuiLayoutSnapshot {
		std::uint64_t Generation = 0;
		GuiViewportConfiguration Viewport;
		std::vector<GuiLayoutNode> Nodes;
		std::unordered_map<ObjectId, std::size_t> NodeByObject;
	};

	struct GuiAccessibilityNode {
		ObjectId Object;
		ObjectId Parent;
		Enums::AccessibilityRole Role = Enums::AccessibilityRole::None;
		std::string Name;
		std::string Value;
		std::uint32_t TraversalOrder = 0;
		bool Enabled = true;
		bool Focused = false;
		bool Pressed = false;
		bool Selected = false;
		bool Editable = false;
		bool ReadOnly = false;
		std::uint32_t Caret = 0;
		std::uint32_t SelectionStart = 0;
		std::uint32_t SelectionLength = 0;
		float ScrollPositionX = 0.0f;
		float ScrollPositionY = 0.0f;
		float ScrollMaximumX = 0.0f;
		float ScrollMaximumY = 0.0f;
	};

	struct GuiAccessibilitySnapshot {
		std::uint64_t Generation = 0;
		std::vector<GuiAccessibilityNode> Nodes;
		std::unordered_map<ObjectId, std::size_t> NodeByObject;
	};

	struct GuiPresentationSnapshot {
		std::uint64_t Generation = 0;
		RenderUiFrame Frame;
	};

	struct GuiRuntimeProfile {
		std::uint64_t SemanticDirtyNanoseconds = 0;
		std::uint64_t ObservationNanoseconds = 0;
		std::uint64_t DirtyMarkingNanoseconds = 0;
		std::uint64_t MeasureNanoseconds = 0;
		std::uint64_t ArrangeNanoseconds = 0;
		std::uint64_t TextShapingNanoseconds = 0;
		std::uint64_t GlyphLookupNanoseconds = 0;
		std::uint64_t GlyphRasterizationNanoseconds = 0;
		std::uint64_t AtlasUpdateNanoseconds = 0;
		std::uint64_t PresentationResolutionNanoseconds = 0;
		std::uint64_t DisplayListNanoseconds = 0;
		std::uint64_t BatchingNanoseconds = 0;
		std::uint64_t AccessibilityNanoseconds = 0;
		std::uint64_t InteractionNanoseconds = 0;
		std::uint64_t SnapshotCommitNanoseconds = 0;
		std::uint64_t FrameConstructionNanoseconds = 0;
		std::uint64_t FrameCopyNanoseconds = 0;
		std::uint64_t PublicationNanoseconds = 0;
		std::uint64_t HitTestNanoseconds = 0;
		std::size_t LayoutRoots = 0;
		std::size_t LayoutNodes = 0;
		std::size_t DirtyObjects = 0;
		std::size_t PresentationNodes = 0;
		std::size_t AccessibilityNodes = 0;
		std::size_t ShapedGlyphs = 0;
		std::size_t DisplayPrimitives = 0;
		std::size_t BatchCount = 0;
		std::size_t TextureUpdates = 0;
		std::size_t TextureUploadBytes = 0;
	};
}
