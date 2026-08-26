#include "gargantuan/services/ActionMap.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/services/UserInputService.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace gargantuan {
	namespace {
		bool IsBindableMouseButton(Enums::UserInputType InputType) {
			return InputType == Enums::UserInputType::MouseButton1 || InputType == Enums::UserInputType::MouseButton2 ||
				   InputType == Enums::UserInputType::MouseButton3;
		}

		void RequireMutationCapability() {
			if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("ActionMap binding mutation requires MutateDataModel");
		}

		void ValidateBindingFields(std::string_view ActionName, float Scale) {
			ValidateProtocolString(ActionName, ActionMap::MaximumActionNameBytes, "Action name");
			if (ActionName.empty()) throw std::invalid_argument("Action name cannot be empty");
			if (!std::isfinite(Scale)) throw std::invalid_argument("Action binding scale must be finite");
		}
	}

	void ActionMap::AttachInputService(const std::shared_ptr<UserInputService> &Service) {
		InputService = Service;
		Reset();
	}

	int ActionMap::AddBinding(Binding Value) {
		RequireMutationCapability();
		if (Bindings.size() >= MaximumBindings) throw std::length_error("ActionMap binding limit reached");
		if (!States.contains(Value.ActionName) && States.size() >= MaximumActions)
			throw std::length_error("ActionMap action limit reached");
		if (NextBindingId <= 0 || NextBindingId == std::numeric_limits<int>::max())
			throw std::overflow_error("ActionMap binding identity is exhausted");
		Value.Id = NextBindingId++;
		const auto Id = Value.Id;
		const auto ActionName = Value.ActionName;
		Bindings.push_back(std::move(Value));
		States.try_emplace(ActionName);
		RefreshAction(ActionName);
		return Id;
	}

	int ActionMap::BindKey(std::string ActionName, Enums::KeyCode KeyCode, float Scale, int Priority, bool Consume) {
		ValidateBindingFields(ActionName, Scale);
		if (KeyCode == Enums::KeyCode::None) throw std::invalid_argument("ActionMap key binding requires a key");
		return AddBinding({
			.ActionName = std::move(ActionName),
			.Kind = BindingKind::Key,
			.KeyCode = KeyCode,
			.ScalarScale = Scale,
			.Priority = Priority,
			.Consume = Consume,
		});
	}

	int ActionMap::BindMouseButton(
		std::string ActionName, Enums::UserInputType InputType, float Scale, int Priority, bool Consume
	) {
		ValidateBindingFields(ActionName, Scale);
		if (!IsBindableMouseButton(InputType))
			throw std::invalid_argument("ActionMap mouse binding requires MouseButton1, MouseButton2, or MouseButton3");
		return AddBinding({
			.ActionName = std::move(ActionName),
			.Kind = BindingKind::MouseButton,
			.InputType = InputType,
			.ScalarScale = Scale,
			.Priority = Priority,
			.Consume = Consume,
		});
	}

	int ActionMap::BindPointerDelta(std::string ActionName, Vector2 Scale, int Priority, bool Consume) {
		ValidateBindingFields(ActionName, Scale.GetX());
		if (!std::isfinite(Scale.GetY())) throw std::invalid_argument("Action binding scale must be finite");
		return AddBinding({
			.ActionName = std::move(ActionName),
			.Kind = BindingKind::PointerDelta,
			.VectorScale = Scale,
			.Priority = Priority,
			.Consume = Consume,
		});
	}

	bool ActionMap::Unbind(int BindingId) {
		RequireMutationCapability();
		auto Found = std::find_if(Bindings.begin(), Bindings.end(), [BindingId](const Binding &Value) {
			return Value.Id == BindingId;
		});
		if (Found == Bindings.end()) return false;
		const auto ActionName = Found->ActionName;
		Bindings.erase(Found);
		RefreshAction(ActionName);
		if (std::ranges::none_of(Bindings, [&](const Binding &Value) { return Value.ActionName == ActionName; }))
			States.erase(ActionName);
		return true;
	}

	void ActionMap::UnbindAction(std::string ActionName) {
		RequireMutationCapability();
		ValidateProtocolString(ActionName, MaximumActionNameBytes, "Action name");
		if (auto Found = States.find(ActionName); Found != States.end()) {
			if (Found->second.Down) ActionEnded->Fire(ActionName);
			if (Found->second.Scalar != 0.0f || Found->second.Vector != Vector2())
				ActionChanged->Fire({ActionName, Vector2()});
		}
		std::erase_if(Bindings, [&](const Binding &Value) { return Value.ActionName == ActionName; });
		States.erase(ActionName);
	}

	bool ActionMap::IsDown(std::string ActionName) {
		auto Found = States.find(ActionName);
		return Found != States.end() && Found->second.Down;
	}

	float ActionMap::GetValue(std::string ActionName) {
		auto Found = States.find(ActionName);
		return Found == States.end() ? 0.0f : Found->second.Scalar;
	}

	Vector2 ActionMap::GetVector(std::string ActionName) {
		auto Found = States.find(ActionName);
		return Found == States.end() ? Vector2() : Found->second.Vector;
	}

	int ActionMap::GetBindingCount() {
		return static_cast<int>(Bindings.size());
	}

	bool ActionMap::IsBindingActive(const Binding &Value) const {
		auto Service = InputService.lock();
		if (!Service) return false;
		if (Value.Kind == BindingKind::Key) return Service->IsKeyDown(Value.KeyCode);
		if (Value.Kind == BindingKind::MouseButton) return Service->IsMouseButtonPressed(Value.InputType);
		return false;
	}

	void ActionMap::RefreshAction(const std::string &ActionName) {
		auto &State = States[ActionName];
		float Scalar = 0.0f;
		for (const auto &Value : Bindings)
			if (Value.ActionName == ActionName && Value.Kind != BindingKind::PointerDelta && IsBindingActive(Value))
				Scalar += Value.ScalarScale;
		Scalar = std::clamp(Scalar, -1.0f, 1.0f);
		const bool Down = Scalar != 0.0f;
		if (!State.Down && Down) ActionBegan->Fire(ActionName);
		if (State.Scalar != Scalar) ActionChanged->Fire({ActionName, Vector2(Scalar, 0.0f)});
		if (State.Down && !Down) ActionEnded->Fire(ActionName);
		State.Scalar = Scalar;
		State.Down = Down;
	}

	bool ActionMap::ProcessEvent(const HostEvent &Event) {
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && !Focus->Focused) {
			Reset();
			return false;
		}

		std::vector<const Binding *> Matching;
		if (const auto *Key = std::get_if<KeyEvent>(&Event); Key && Key->Logical != LogicalKey::Unknown) {
			const auto KeyCode = static_cast<Enums::KeyCode>(Key->Logical);
			for (const auto &Value : Bindings)
				if (Value.Kind == BindingKind::Key && Value.KeyCode == KeyCode) Matching.push_back(&Value);
		} else if (const auto *Gamepad = std::get_if<GamepadButtonEvent>(&Event)) {
			auto KeyCode = InputObject::GetGamepadKeyCode(Gamepad->Button);
			if (!KeyCode) return false;
			for (const auto &Value : Bindings)
				if (Value.Kind == BindingKind::Key && Value.KeyCode == *KeyCode) Matching.push_back(&Value);
		} else if (const auto *Button = std::get_if<PointerButtonEvent>(&Event)) {
			Enums::UserInputType InputType = Enums::UserInputType::None;
			if (Button->Button == PointerButton::Left)
				InputType = Enums::UserInputType::MouseButton1;
			else if (Button->Button == PointerButton::Right)
				InputType = Enums::UserInputType::MouseButton2;
			else if (Button->Button == PointerButton::Middle)
				InputType = Enums::UserInputType::MouseButton3;
			for (const auto &Value : Bindings)
				if (Value.Kind == BindingKind::MouseButton && Value.InputType == InputType) Matching.push_back(&Value);
		} else if (const auto *Move = std::get_if<PointerMoveEvent>(&Event)) {
			for (const auto &Value : Bindings)
				if (Value.Kind == BindingKind::PointerDelta) Matching.push_back(&Value);
			std::ranges::sort(Matching, [](const Binding *Left, const Binding *Right) {
				return Left->Priority != Right->Priority ? Left->Priority > Right->Priority : Left->Id < Right->Id;
			});
			bool Consumed = false;
			std::vector<std::string> Actions;
			std::unordered_set<std::string> SeenActions;
			for (const auto *Value : Matching) {
				auto &State = States[Value->ActionName];
				State.Vector = State.Vector +
							   Vector2(
								   Move->Delta.X * Value->VectorScale.GetX(), Move->Delta.Y * Value->VectorScale.GetY()
							   );
				if (SeenActions.insert(Value->ActionName).second) Actions.push_back(Value->ActionName);
				Consumed = Consumed || Value->Consume;
			}
			for (const auto &ActionName : Actions)
				ActionChanged->Fire({ActionName, States[ActionName].Vector});
			return Consumed;
		}

		std::ranges::sort(Matching, [](const Binding *Left, const Binding *Right) {
			return Left->Priority != Right->Priority ? Left->Priority > Right->Priority : Left->Id < Right->Id;
		});
		std::vector<std::string> Actions;
		std::unordered_set<std::string> SeenActions;
		bool Consumed = false;
		for (const auto *Value : Matching) {
			if (SeenActions.insert(Value->ActionName).second) Actions.push_back(Value->ActionName);
			Consumed = Consumed || Value->Consume;
		}
		for (const auto &ActionName : Actions)
			RefreshAction(ActionName);
		return Consumed;
	}

	void ActionMap::ProcessConsumedRelease(const HostEvent &Event) {
		std::optional<Enums::KeyCode> ReleasedKey;
		std::optional<Enums::UserInputType> ReleasedMouseButton;
		if (const auto *Key = std::get_if<KeyEvent>(&Event);
			Key && Key->State == ButtonState::Released && Key->Logical != LogicalKey::Unknown)
			ReleasedKey = static_cast<Enums::KeyCode>(Key->Logical);
		else if (const auto *Gamepad = std::get_if<GamepadButtonEvent>(&Event);
				 Gamepad && Gamepad->State == ButtonState::Released)
			ReleasedKey = InputObject::GetGamepadKeyCode(Gamepad->Button);
		else if (const auto *Button = std::get_if<PointerButtonEvent>(&Event);
				 Button && Button->State == ButtonState::Released) {
			if (Button->Button == PointerButton::Left)
				ReleasedMouseButton = Enums::UserInputType::MouseButton1;
			else if (Button->Button == PointerButton::Right)
				ReleasedMouseButton = Enums::UserInputType::MouseButton2;
			else if (Button->Button == PointerButton::Middle)
				ReleasedMouseButton = Enums::UserInputType::MouseButton3;
		}
		if (!ReleasedKey && !ReleasedMouseButton) return;
		std::vector<std::string> Actions;
		for (const auto &Binding : Bindings) {
			const bool MatchesKey = ReleasedKey && Binding.Kind == BindingKind::Key && Binding.KeyCode == *ReleasedKey;
			const bool MatchesButton = ReleasedMouseButton && Binding.Kind == BindingKind::MouseButton &&
									   Binding.InputType == *ReleasedMouseButton;
			if ((MatchesKey || MatchesButton) && !std::ranges::contains(Actions, Binding.ActionName))
				Actions.push_back(Binding.ActionName);
		}
		std::ranges::sort(Actions);
		for (const auto &ActionName : Actions)
			RefreshAction(ActionName);
	}

	void ActionMap::EndFrame() {
		for (auto &[_, State] : States)
			State.Vector = Vector2();
	}

	void ActionMap::Reset() {
		std::vector<std::string> ActionNames;
		ActionNames.reserve(States.size());
		for (const auto &[ActionName, _] : States)
			ActionNames.push_back(ActionName);
		std::ranges::sort(ActionNames);
		for (const auto &ActionName : ActionNames) {
			auto &State = States[ActionName];
			if (State.Down) ActionEnded->Fire(ActionName);
			if (State.Scalar != 0.0f || State.Vector != Vector2()) ActionChanged->Fire({ActionName, Vector2()});
			State = {};
		}
	}
}
