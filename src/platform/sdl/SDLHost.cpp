#include "platform/sdl/SDLHost.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace gargantuan {
	namespace {
		std::optional<LogicalKey> TranslateLogicalKey(SDL_Keycode Key) {
			switch (Key) {
#define G_KEY(SDLName, GargantuanName) case SDLName: return LogicalKey::GargantuanName
			G_KEY(SDLK_BACKSPACE, Backspace); G_KEY(SDLK_TAB, Tab); G_KEY(SDLK_RETURN, Return);
			G_KEY(SDLK_RETURN2, Return); G_KEY(SDLK_PAUSE, Pause); G_KEY(SDLK_ESCAPE, Escape);
			G_KEY(SDLK_SPACE, Space); G_KEY(SDLK_DBLAPOSTROPHE, QuotedDouble); G_KEY(SDLK_HASH, Hash);
			G_KEY(SDLK_DOLLAR, Dollar); G_KEY(SDLK_PERCENT, Percent); G_KEY(SDLK_AMPERSAND, Ampersand);
			G_KEY(SDLK_APOSTROPHE, Quote); G_KEY(SDLK_LEFTPAREN, LeftParenthesis); G_KEY(SDLK_RIGHTPAREN, RightParenthesis);
			G_KEY(SDLK_ASTERISK, Asterisk); G_KEY(SDLK_PLUS, Plus); G_KEY(SDLK_COMMA, Comma);
			G_KEY(SDLK_MINUS, Minus); G_KEY(SDLK_PERIOD, Period); G_KEY(SDLK_SLASH, Slash);
			G_KEY(SDLK_0, Zero); G_KEY(SDLK_1, One); G_KEY(SDLK_2, Two); G_KEY(SDLK_3, Three);
			G_KEY(SDLK_4, Four); G_KEY(SDLK_5, Five); G_KEY(SDLK_6, Six); G_KEY(SDLK_7, Seven);
			G_KEY(SDLK_8, Eight); G_KEY(SDLK_9, Nine); G_KEY(SDLK_COLON, Colon); G_KEY(SDLK_SEMICOLON, Semicolon);
			G_KEY(SDLK_LESS, LessThan); G_KEY(SDLK_EQUALS, Equals); G_KEY(SDLK_GREATER, GreaterThan);
			G_KEY(SDLK_QUESTION, Question); G_KEY(SDLK_AT, At); G_KEY(SDLK_LEFTBRACKET, LeftBracket);
			G_KEY(SDLK_BACKSLASH, BackSlash); G_KEY(SDLK_RIGHTBRACKET, RightBracket); G_KEY(SDLK_CARET, Caret);
			G_KEY(SDLK_UNDERSCORE, Underscore); G_KEY(SDLK_GRAVE, Backquote);
			G_KEY(SDLK_A, A); G_KEY(SDLK_B, B); G_KEY(SDLK_C, C); G_KEY(SDLK_D, D); G_KEY(SDLK_E, E);
			G_KEY(SDLK_F, F); G_KEY(SDLK_G, G); G_KEY(SDLK_H, H); G_KEY(SDLK_I, I); G_KEY(SDLK_J, J);
			G_KEY(SDLK_K, K); G_KEY(SDLK_L, L); G_KEY(SDLK_M, M); G_KEY(SDLK_N, N); G_KEY(SDLK_O, O);
			G_KEY(SDLK_P, P); G_KEY(SDLK_Q, Q); G_KEY(SDLK_R, R); G_KEY(SDLK_S, S); G_KEY(SDLK_T, T);
			G_KEY(SDLK_U, U); G_KEY(SDLK_V, V); G_KEY(SDLK_W, W); G_KEY(SDLK_X, X); G_KEY(SDLK_Y, Y); G_KEY(SDLK_Z, Z);
			G_KEY(SDLK_LEFTBRACE, LeftCurly); G_KEY(SDLK_PIPE, Pipe); G_KEY(SDLK_RIGHTBRACE, RightCurly);
			G_KEY(SDLK_TILDE, Tilde); G_KEY(SDLK_DELETE, Delete);
			G_KEY(SDLK_KP_0, KeypadZero); G_KEY(SDLK_KP_1, KeypadOne); G_KEY(SDLK_KP_2, KeypadTwo);
			G_KEY(SDLK_KP_3, KeypadThree); G_KEY(SDLK_KP_4, KeypadFour); G_KEY(SDLK_KP_5, KeypadFive);
			G_KEY(SDLK_KP_6, KeypadSix); G_KEY(SDLK_KP_7, KeypadSeven); G_KEY(SDLK_KP_8, KeypadEight);
			G_KEY(SDLK_KP_9, KeypadNine); G_KEY(SDLK_KP_PERIOD, KeypadPeriod); G_KEY(SDLK_KP_DIVIDE, KeypadDivide);
			G_KEY(SDLK_KP_MULTIPLY, KeypadMultiply); G_KEY(SDLK_KP_MINUS, KeypadMinus); G_KEY(SDLK_KP_PLUS, KeypadPlus);
			G_KEY(SDLK_KP_ENTER, KeypadEnter); G_KEY(SDLK_KP_EQUALS, KeypadEquals); G_KEY(SDLK_UP, Up);
			G_KEY(SDLK_DOWN, Down); G_KEY(SDLK_RIGHT, Right); G_KEY(SDLK_LEFT, Left); G_KEY(SDLK_INSERT, Insert);
			G_KEY(SDLK_HOME, Home); G_KEY(SDLK_END, End); G_KEY(SDLK_PAGEUP, PageUp); G_KEY(SDLK_PAGEDOWN, PageDown);
			G_KEY(SDLK_F1, F1); G_KEY(SDLK_F2, F2); G_KEY(SDLK_F3, F3); G_KEY(SDLK_F4, F4); G_KEY(SDLK_F5, F5);
			G_KEY(SDLK_F6, F6); G_KEY(SDLK_F7, F7); G_KEY(SDLK_F8, F8); G_KEY(SDLK_F9, F9); G_KEY(SDLK_F10, F10);
			G_KEY(SDLK_F11, F11); G_KEY(SDLK_F12, F12); G_KEY(SDLK_F13, F13); G_KEY(SDLK_F14, F14); G_KEY(SDLK_F15, F15);
			G_KEY(SDLK_NUMLOCKCLEAR, NumLock); G_KEY(SDLK_CAPSLOCK, CapsLock); G_KEY(SDLK_SCROLLLOCK, ScrollLock);
			G_KEY(SDLK_RSHIFT, RightShift); G_KEY(SDLK_LSHIFT, LeftShift); G_KEY(SDLK_RCTRL, RightControl);
			G_KEY(SDLK_LCTRL, LeftControl); G_KEY(SDLK_RALT, RightAlt); G_KEY(SDLK_LALT, LeftAlt);
			G_KEY(SDLK_RMETA, RightMeta); G_KEY(SDLK_LMETA, LeftMeta); G_KEY(SDLK_LGUI, LeftSuper);
			G_KEY(SDLK_RGUI, RightSuper); G_KEY(SDLK_MODE, Mode); G_KEY(SDLK_MULTI_KEY_COMPOSE, Compose);
			G_KEY(SDLK_HELP, Help); G_KEY(SDLK_PRINTSCREEN, Print); G_KEY(SDLK_SYSREQ, SysReq);
			G_KEY(SDLK_MENU, Menu); G_KEY(SDLK_POWER, Power); G_KEY(SDLK_CURRENCYUNIT, Euro); G_KEY(SDLK_UNDO, Undo);
#undef G_KEY
			default: return std::nullopt;
			}
		}

		PhysicalKey TranslatePhysicalKey(SDL_Scancode Key) {
			const auto Value = static_cast<std::uint16_t>(Key);
			if ((Value >= static_cast<std::uint16_t>(PhysicalKey::A) && Value <= static_cast<std::uint16_t>(PhysicalKey::Space)) ||
				(Value >= static_cast<std::uint16_t>(PhysicalKey::Right) && Value <= static_cast<std::uint16_t>(PhysicalKey::Up)) ||
				(Value >= static_cast<std::uint16_t>(PhysicalKey::LeftControl) && Value <= static_cast<std::uint16_t>(PhysicalKey::RightMeta)))
				return static_cast<PhysicalKey>(Value);
			return PhysicalKey::Unknown;
		}

		KeyModifier TranslateModifiers(SDL_Keymod Modifiers) {
			KeyModifier Result = KeyModifier::None;
			if ((Modifiers & SDL_KMOD_SHIFT) != 0) Result = Result | KeyModifier::Shift;
			if ((Modifiers & SDL_KMOD_CTRL) != 0) Result = Result | KeyModifier::Control;
			if ((Modifiers & SDL_KMOD_ALT) != 0) Result = Result | KeyModifier::Alt;
			if ((Modifiers & SDL_KMOD_GUI) != 0) Result = Result | KeyModifier::Meta;
			if ((Modifiers & SDL_KMOD_CAPS) != 0) Result = Result | KeyModifier::CapsLock;
			if ((Modifiers & SDL_KMOD_NUM) != 0) Result = Result | KeyModifier::NumLock;
			return Result;
		}

		std::optional<PointerButton> TranslatePointerButton(std::uint8_t Button) {
			switch (Button) {
			case SDL_BUTTON_LEFT: return PointerButton::Left;
			case SDL_BUTTON_RIGHT: return PointerButton::Right;
			case SDL_BUTTON_MIDDLE: return PointerButton::Middle;
			case SDL_BUTTON_X1: return PointerButton::Auxiliary1;
			case SDL_BUTTON_X2: return PointerButton::Auxiliary2;
			default: return std::nullopt;
			}
		}

		std::optional<GamepadButton> TranslateGamepadButton(std::uint8_t Button) {
			if (Button > SDL_GAMEPAD_BUTTON_TOUCHPAD) return std::nullopt;
			return static_cast<GamepadButton>(Button);
		}

		std::optional<GamepadAxis> TranslateGamepadAxis(std::uint8_t Axis) {
			if (Axis >= SDL_GAMEPAD_AXIS_COUNT) return std::nullopt;
			return static_cast<GamepadAxis>(Axis);
		}
	}

	std::optional<HostEvent> TranslateSDLEvent(const SDL_Event &Event) {
		switch (Event.type) {
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			const auto Physical = TranslatePhysicalKey(Event.key.scancode);
			const auto Logical = TranslateLogicalKey(Event.key.key).value_or(LogicalKey::Unknown);
			if (Physical == PhysicalKey::Unknown && Logical == LogicalKey::Unknown) return std::nullopt;
			return KeyEvent{
				.Device = {Event.key.which}, .Physical = Physical, .Logical = Logical,
				.Modifiers = TranslateModifiers(Event.key.mod),
				.State = Event.key.down ? ButtonState::Pressed : ButtonState::Released, .Repeat = Event.key.repeat,
			};
		}
		case SDL_EVENT_MOUSE_MOTION:
			return PointerMoveEvent{{Event.motion.which}, {Event.motion.x, Event.motion.y}, {Event.motion.xrel, Event.motion.yrel}};
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			auto Button = TranslatePointerButton(Event.button.button);
			if (!Button) return std::nullopt;
			return PointerButtonEvent{
				{Event.button.which}, *Button, Event.button.down ? ButtonState::Pressed : ButtonState::Released,
				{Event.button.x, Event.button.y},
			};
		}
		case SDL_EVENT_MOUSE_WHEEL: {
			const float Direction = Event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
			return WheelEvent{{Event.wheel.which}, {Event.wheel.mouse_x, Event.wheel.mouse_y},
				{Event.wheel.x * Direction, Event.wheel.y * Direction}};
		}
		case SDL_EVENT_TEXT_INPUT: {
			if (!Event.text.text) return std::nullopt;
			auto Text = BoundedUtf8::From(Event.text.text);
			if (!Text) return std::nullopt;
			return TextInputEvent{{0}, *Text};
		}
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP: {
			if (Event.gbutton.which == 0) return std::nullopt;
			auto Button = TranslateGamepadButton(Event.gbutton.button);
			if (!Button) return std::nullopt;
			return GamepadButtonEvent{{Event.gbutton.which}, *Button,
				Event.gbutton.down ? ButtonState::Pressed : ButtonState::Released};
		}
		case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
			if (Event.gaxis.which == 0) return std::nullopt;
			auto Axis = TranslateGamepadAxis(Event.gaxis.axis);
			if (!Axis) return std::nullopt;
			const float Value = Event.gaxis.value < 0 ? static_cast<float>(Event.gaxis.value) / 32768.0f
				: static_cast<float>(Event.gaxis.value) / 32767.0f;
			return GamepadAxisEvent{{Event.gaxis.which}, *Axis, std::clamp(Value, -1.0f, 1.0f)};
		}
		case SDL_EVENT_WINDOW_RESIZED: {
			auto *Window = SDL_GetWindowFromEvent(&Event);
			if (!Window) return std::nullopt;
			int Width = 0;
			int Height = 0;
			if (!SDL_GetWindowSizeInPixels(Window, &Width, &Height) || Width < 1 || Height < 1) return std::nullopt;
			return WindowResizeEvent{static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height)};
		}
		case SDL_EVENT_WINDOW_FOCUS_GAINED: return FocusEvent{true};
		case SDL_EVENT_WINDOW_FOCUS_LOST: return FocusEvent{false};
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		case SDL_EVENT_QUIT: return WindowCloseEvent{};
		default: return std::nullopt;
		}
	}

	bool SDLHost::PollEvent(HostEvent &Event) {
		SDL_Event BackendEvent;
		while (SDL_PollEvent(&BackendEvent)) {
			switch (BackendEvent.type) {
			case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: ActiveWindowId = BackendEvent.key.windowID; break;
			case SDL_EVENT_MOUSE_MOTION: ActiveWindowId = BackendEvent.motion.windowID; break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: ActiveWindowId = BackendEvent.button.windowID; break;
			case SDL_EVENT_MOUSE_WHEEL: ActiveWindowId = BackendEvent.wheel.windowID; break;
			case SDL_EVENT_WINDOW_FOCUS_GAINED: case SDL_EVENT_WINDOW_FOCUS_LOST:
			case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				ActiveWindowId = BackendEvent.window.windowID; break;
			default: break;
			}
			auto Translated = TranslateSDLEvent(BackendEvent);
			if (!Translated) continue;
			Event = std::move(*Translated);
			return true;
		}
		return false;
	}

	void SDLHost::Apply(const HostCommand &Command) const {
		std::visit([this](const auto &Value) {
			using CommandType = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<CommandType, SetRelativePointerMode>) {
				auto *Window = ActiveWindowId == 0 ? nullptr : SDL_GetWindowFromID(ActiveWindowId);
				if (Window) SDL_SetWindowRelativeMouseMode(Window, Value.Enabled);
			}
		}, Command);
	}
}
