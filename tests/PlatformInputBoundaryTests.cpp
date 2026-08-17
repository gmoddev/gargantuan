#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/InputObject.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "platform/sdl/SDLHost.hpp"

#include <SDL3/SDL.h>

#include <cassert>
#include <string>
#include <variant>

using namespace gargantuan;

namespace {
	void TestBoundedText() {
		auto Maximum = BoundedUtf8::From(std::string(MAX_TEXT_INPUT_BYTES, 'a'));
		assert(Maximum && Maximum->View().size() == MAX_TEXT_INPUT_BYTES);
		assert(!BoundedUtf8::From(std::string(MAX_TEXT_INPUT_BYTES + 1, 'a')));
		assert(!BoundedUtf8::From(std::string("\xc0\x80", 2)));
		auto Unicode = BoundedUtf8::From("Gargantuan \xf0\x9f\xa6\x96");
		assert(Unicode && Unicode->View() == "Gargantuan \xf0\x9f\xa6\x96");
	}

	void TestSDLTranslation() {
		SDL_Event Backend{};
		Backend.type = SDL_EVENT_KEY_DOWN;
		Backend.key.type = SDL_EVENT_KEY_DOWN;
		Backend.key.which = 7;
		Backend.key.scancode = SDL_SCANCODE_W;
		Backend.key.key = SDLK_W;
		Backend.key.mod = static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_CTRL);
		Backend.key.down = true;
		Backend.key.repeat = true;
		auto Event = TranslateSDLEvent(Backend);
		assert(Event && std::holds_alternative<KeyEvent>(*Event));
		const auto &Key = std::get<KeyEvent>(*Event);
		assert(Key.Device.Value == 7);
		assert(Key.Physical == PhysicalKey::W && Key.Logical == LogicalKey::W);
		assert(Key.State == ButtonState::Pressed && Key.Repeat);
		assert(HasModifier(Key.Modifiers, KeyModifier::Shift));
		assert(HasModifier(Key.Modifiers, KeyModifier::Control));

		Backend.key.key = SDLK_UNKNOWN;
		Event = TranslateSDLEvent(Backend);
		assert(Event && std::get<KeyEvent>(*Event).Physical == PhysicalKey::W);
		assert(std::get<KeyEvent>(*Event).Logical == LogicalKey::Unknown);
		assert(!InputObject::FromHostEvent(*Event));
		Backend.key.scancode = SDL_SCANCODE_UNKNOWN;
		assert(!TranslateSDLEvent(Backend));

		Backend = {};
		Backend.type = SDL_EVENT_MOUSE_WHEEL;
		Backend.wheel.type = SDL_EVENT_MOUSE_WHEEL;
		Backend.wheel.which = 3;
		Backend.wheel.x = 1.5f;
		Backend.wheel.y = -2.0f;
		Backend.wheel.mouse_x = 40.0f;
		Backend.wheel.mouse_y = 50.0f;
		Backend.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
		Event = TranslateSDLEvent(Backend);
		const auto &Wheel = std::get<WheelEvent>(*Event);
		assert(Wheel.Delta.X == -1.5f && Wheel.Delta.Y == 2.0f);

		Backend = {};
		Backend.type = SDL_EVENT_TEXT_INPUT;
		Backend.text.type = SDL_EVENT_TEXT_INPUT;
		Backend.text.text = "typed";
		Event = TranslateSDLEvent(Backend);
		assert(Event && std::get<TextInputEvent>(*Event).Text.View() == "typed");
		Backend.text.text = "\xc0\x80";
		assert(!TranslateSDLEvent(Backend));

		Backend = {};
		Backend.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
		Backend.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
		Backend.gaxis.which = 5;
		Backend.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
		Backend.gaxis.value = -32768;
		Event = TranslateSDLEvent(Backend);
		assert(Event && std::get<GamepadAxisEvent>(*Event).Value == -1.0f);
		Backend.gaxis.which = 0;
		assert(!TranslateSDLEvent(Backend));
	}

	void TestEngineOwnedInputObjects() {
		HostEvent KeyDown = KeyEvent{
			.Device = {1}, .Physical = PhysicalKey::Space, .Logical = LogicalKey::Space,
			.State = ButtonState::Pressed,
		};
		auto Input = InputObject::FromHostEvent(KeyDown);
		assert(Input && Input->GetUserInputType() == Enums::UserInputType::Keyboard);
		assert(Input->GetUserInputState() == Enums::UserInputState::Begin);
		assert(Input->GetKeyCode() == Enums::KeyCode::Space);

		HostEvent Auxiliary = PointerButtonEvent{{1}, PointerButton::Auxiliary1, ButtonState::Pressed, {1.0f, 2.0f}};
		assert(!InputObject::FromHostEvent(Auxiliary));

		UserInputService Service;
		assert(!Service.ProcessEvent(KeyDown));
		assert(Service.IsKeyDown(Enums::KeyCode::Space));
		HostEvent KeyUp = KeyEvent{
			.Device = {1}, .Physical = PhysicalKey::Space, .Logical = LogicalKey::Space,
			.State = ButtonState::Released,
		};
		assert(!Service.ProcessEvent(KeyUp));
		assert(!Service.IsKeyDown(Enums::KeyCode::Space));

		HostEvent MouseDown = PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Pressed, {10.0f, 20.0f}};
		assert(!Service.ProcessEvent(MouseDown));
		assert(Service.IsMouseButtonPressed(Enums::UserInputType::MouseButton1));
		HostEvent MouseUp = PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Released, {10.0f, 20.0f}};
		assert(!Service.ProcessEvent(MouseUp));
		assert(!Service.IsMouseButtonPressed(Enums::UserInputType::MouseButton1));

		HostEvent Motion = PointerMoveEvent{{1}, {30.0f, 40.0f}, {3.0f, 4.0f}};
		assert(!Service.ProcessEvent(Motion));
		assert(Service.GetMouseLocation().GetX() == 30.0f && Service.GetMouseLocation().GetY() == 40.0f);
		assert(Service.GetMouseDelta().GetX() == 3.0f && Service.GetMouseDelta().GetY() == 4.0f);

		(void)Service.ProcessEvent(KeyDown);
		(void)Service.ProcessEvent(PointerButtonEvent{
			{1}, PointerButton::Right, ButtonState::Pressed, {30.0f, 40.0f}
		});
		assert(Service.IsKeyDown(Enums::KeyCode::Space));
		assert(Service.IsMouseButtonPressed(Enums::UserInputType::MouseButton2));
		(void)Service.ProcessEvent(FocusEvent{false});
		assert(!Service.IsKeyDown(Enums::KeyCode::Space));
		assert(!Service.IsMouseButtonPressed(Enums::UserInputType::MouseButton2));
		assert(Service.GetMouseDelta().GetX() == 0.0f && Service.GetMouseDelta().GetY() == 0.0f);
	}

	void TestCameraHostCommands() {
		auto CameraValue = std::make_shared<Camera>();
		HostEvent RightDown = PointerButtonEvent{{1}, PointerButton::Right, ButtonState::Pressed, {0.0f, 0.0f}};
		auto Command = CameraValue->ProcessEvent(RightDown);
		assert(Command && std::get<SetRelativePointerMode>(*Command).Enabled);
		const auto BeforeLook = CameraValue->GetCFrame();
		(void)CameraValue->ProcessEvent(PointerMoveEvent{{1}, {100.0f, 50.0f}, {10.0f, -5.0f}});
		CameraValue->Step(0.0f);
		assert(!CameraValue->GetCFrame().FuzzyEq(BeforeLook));
		(void)CameraValue->ProcessEvent(KeyEvent{
			.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W,
			.State = ButtonState::Pressed,
		});
		const auto BeforeMove = CameraValue->GetCFrame();
		CameraValue->Step(0.1f);
		assert(!CameraValue->GetCFrame().FuzzyEq(BeforeMove));
		(void)CameraValue->ProcessEvent(KeyEvent{
			.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W,
			.State = ButtonState::Released,
		});
		const auto CheckMovementKey = [&](PhysicalKey Physical, LogicalKey Logical) {
			const auto Before = CameraValue->GetCFrame();
			(void)CameraValue->ProcessEvent(KeyEvent{
				.Device = {1}, .Physical = Physical, .Logical = Logical, .State = ButtonState::Pressed,
			});
			CameraValue->Step(0.1f);
			(void)CameraValue->ProcessEvent(KeyEvent{
				.Device = {1}, .Physical = Physical, .Logical = Logical, .State = ButtonState::Released,
			});
			assert(!CameraValue->GetCFrame().FuzzyEq(Before));
		};
		CheckMovementKey(PhysicalKey::A, LogicalKey::A);
		CheckMovementKey(PhysicalKey::D, LogicalKey::D);
		CheckMovementKey(PhysicalKey::Space, LogicalKey::Space);
		CheckMovementKey(PhysicalKey::LeftShift, LogicalKey::LeftShift);
		(void)CameraValue->ProcessEvent(KeyEvent{
			.Device = {1}, .Physical = PhysicalKey::W, .Logical = LogicalKey::W,
			.State = ButtonState::Pressed,
		});
		(void)CameraValue->ProcessEvent(PointerMoveEvent{{1}, {200.0f, 100.0f}, {8.0f, 4.0f}});
		HostEvent LostFocus = FocusEvent{false};
		Command = CameraValue->ProcessEvent(LostFocus);
		assert(Command && !std::get<SetRelativePointerMode>(*Command).Enabled);
		const auto AfterFocusLoss = CameraValue->GetCFrame();
		CameraValue->Step(0.1f);
		assert(CameraValue->GetCFrame().FuzzyEq(AfterFocusLoss));
		Command = CameraValue->ProcessEvent(PointerButtonEvent{
			{1}, PointerButton::Right, ButtonState::Released, {0.0f, 0.0f}
		});
		assert(Command && !std::get<SetRelativePointerMode>(*Command).Enabled);
	}
}

int main() {
	BootstrapNativeRuntimeSchema();
	TestBoundedText();
	TestSDLTranslation();
	TestEngineOwnedInputObjects();
	TestCameraHostCommands();
	return 0;
}
