#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace gargantuan {
	inline constexpr std::size_t MAX_TEXT_INPUT_BYTES = 63;

	enum class ButtonState : std::uint8_t { Released, Pressed };

	enum class PhysicalKey : std::uint16_t {
		Unknown = 0,
		A = 4,
		B,
		C,
		D,
		E,
		F,
		G,
		H,
		I,
		J,
		K,
		L,
		M,
		N,
		O,
		P,
		Q,
		R,
		S,
		T,
		U,
		V,
		W,
		X,
		Y,
		Z,
		One = 30,
		Two,
		Three,
		Four,
		Five,
		Six,
		Seven,
		Eight,
		Nine,
		Zero,
		Return,
		Escape,
		Backspace,
		Tab,
		Space,
		Right = 79,
		Left,
		Down,
		Up,
		LeftControl = 224,
		LeftShift,
		LeftAlt,
		LeftMeta,
		RightControl,
		RightShift,
		RightAlt,
		RightMeta,
	};

	// Logical keys preserve the existing developer-visible KeyCode values while
	// remaining independent of SDL's keycode representation.
	enum class LogicalKey : std::uint16_t {
		Backspace = 8, Tab = 9, Clear = 12, Return = 13, Pause = 19, Escape = 27, Space = 32,
		QuotedDouble = 34, Hash = 35, Dollar = 36, Percent = 37, Ampersand = 38, Quote = 39,
		LeftParenthesis = 40, RightParenthesis = 41, Asterisk = 42, Plus = 43, Comma = 44,
		Minus = 45, Period = 46, Slash = 47, Zero = 48, One = 49, Two = 50, Three = 51,
		Four = 52, Five = 53, Six = 54, Seven = 55, Eight = 56, Nine = 57, Colon = 58,
		Semicolon = 59, LessThan = 60, Equals = 61, GreaterThan = 62, Question = 63, At = 64,
		LeftBracket = 91, BackSlash = 92, RightBracket = 93, Caret = 94, Underscore = 95,
		Backquote = 96, A = 97, B = 98, C = 99, D = 100, E = 101, F = 102, G = 103,
		H = 104, I = 105, J = 106, K = 107, L = 108, M = 109, N = 110, O = 111,
		P = 112, Q = 113, R = 114, S = 115, T = 116, U = 117, V = 118, W = 119,
		X = 120, Y = 121, Z = 122, LeftCurly = 123, Pipe = 124, RightCurly = 125,
		Tilde = 126, Delete = 127, KeypadZero = 256, KeypadOne = 257, KeypadTwo = 258,
		KeypadThree = 259, KeypadFour = 260, KeypadFive = 261, KeypadSix = 262,
		KeypadSeven = 263, KeypadEight = 264, KeypadNine = 265, KeypadPeriod = 266,
		KeypadDivide = 267, KeypadMultiply = 268, KeypadMinus = 269, KeypadPlus = 270,
		KeypadEnter = 271, KeypadEquals = 272, Up = 273, Down = 274, Right = 275, Left = 276,
		Insert = 277, Home = 278, End = 279, PageUp = 280, PageDown = 281, F1 = 282,
		F2 = 283, F3 = 284, F4 = 285, F5 = 286, F6 = 287, F7 = 288, F8 = 289,
		F9 = 290, F10 = 291, F11 = 292, F12 = 293, F13 = 294, F14 = 295, F15 = 296,
		NumLock = 300, CapsLock = 301, ScrollLock = 302, RightShift = 303, LeftShift = 304,
		RightControl = 305, LeftControl = 306, RightAlt = 307, LeftAlt = 308, RightMeta = 309,
		LeftMeta = 310, LeftSuper = 311, RightSuper = 312, Mode = 313, Compose = 314,
		Help = 315, Print = 316, SysReq = 317, Break = 318, Menu = 319, Power = 320,
		Euro = 321, Undo = 322, Unknown = 2048,
	};

	enum class KeyModifier : std::uint8_t {
		None = 0,
		Shift = 1 << 0,
		Control = 1 << 1,
		Alt = 1 << 2,
		Meta = 1 << 3,
		CapsLock = 1 << 4,
		NumLock = 1 << 5,
	};

	[[nodiscard]] constexpr KeyModifier operator|(KeyModifier Left, KeyModifier Right) {
		return static_cast<KeyModifier>(static_cast<std::uint8_t>(Left) | static_cast<std::uint8_t>(Right));
	}

	[[nodiscard]] constexpr bool HasModifier(KeyModifier Modifiers, KeyModifier Modifier) {
		return (static_cast<std::uint8_t>(Modifiers) & static_cast<std::uint8_t>(Modifier)) != 0;
	}

	struct KeyboardDeviceId { std::uint32_t Value = 0; };
	struct PointerDeviceId { std::uint32_t Value = 0; };
	struct GamepadDeviceId { std::uint32_t Value = 0; };
	struct HostPoint { float X = 0.0f; float Y = 0.0f; };

	struct KeyEvent {
		KeyboardDeviceId Device;
		PhysicalKey Physical = PhysicalKey::Unknown;
		LogicalKey Logical = LogicalKey::Unknown;
		KeyModifier Modifiers = KeyModifier::None;
		ButtonState State = ButtonState::Released;
		bool Repeat = false;
	};

	enum class PointerButton : std::uint8_t { Left, Right, Middle, Auxiliary1, Auxiliary2 };
	struct PointerMoveEvent { PointerDeviceId Device; HostPoint Position; HostPoint Delta; };
	struct PointerButtonEvent {
		PointerDeviceId Device;
		PointerButton Button = PointerButton::Left;
		ButtonState State = ButtonState::Released;
		HostPoint Position;
	};
	struct WheelEvent { PointerDeviceId Device; HostPoint Position; HostPoint Delta; };

	class BoundedUtf8 final {
	  public:
		[[nodiscard]] static std::optional<BoundedUtf8> From(std::string_view Text);
		[[nodiscard]] std::string_view View() const { return {Bytes.data(), Size}; }

	  private:
		std::array<char, MAX_TEXT_INPUT_BYTES> Bytes{};
		std::uint8_t Size = 0;
	};

	struct TextInputEvent { KeyboardDeviceId Device; BoundedUtf8 Text; };

	enum class GamepadButton : std::uint8_t {
		South, East, West, North, Back, Guide, Start, LeftStick, RightStick,
		LeftShoulder, RightShoulder, DPadUp, DPadDown, DPadLeft, DPadRight,
		Misc, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2, Touchpad,
	};
	enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };
	struct GamepadButtonEvent {
		GamepadDeviceId Device;
		GamepadButton Button = GamepadButton::South;
		ButtonState State = ButtonState::Released;
	};
	struct GamepadAxisEvent {
		GamepadDeviceId Device;
		GamepadAxis Axis = GamepadAxis::LeftX;
		float Value = 0.0f;
	};
	struct WindowResizeEvent { std::uint32_t Width = 0; std::uint32_t Height = 0; };
	struct FocusEvent { bool Focused = false; };
	struct WindowCloseEvent {};

	using HostEvent = std::variant<
		KeyEvent, PointerMoveEvent, PointerButtonEvent, WheelEvent, TextInputEvent,
		GamepadButtonEvent, GamepadAxisEvent, WindowResizeEvent, FocusEvent, WindowCloseEvent
	>;

	struct SetRelativePointerMode { bool Enabled = false; };
	using HostCommand = std::variant<SetRelativePointerMode>;
	struct HostEventResult {
		bool Consumed = false;
		std::optional<HostCommand> Command;
	};
}
