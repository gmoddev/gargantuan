#include "gargantuan/classes/Constraint.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include <memory>

namespace gargantuan {
	std::tuple<std::shared_ptr<BasePart>, std::shared_ptr<BasePart>> Constraint::GetActiveParts() const {
		if (!Attachment0 || !*Attachment0 || !Attachment1 || !*Attachment1) return {nullptr, nullptr};
		auto maybePart0 = (*Attachment0)->GetParent();
		if (!maybePart0 || !WorldRoot::GetEffectiveWorld(**maybePart0)) return {nullptr, nullptr};
		auto part0 = std::dynamic_pointer_cast<BasePart>(*maybePart0);
		if (!part0) return {nullptr, nullptr};

		auto maybePart1 = (*Attachment1)->GetParent();
		if (!maybePart1 || !WorldRoot::GetEffectiveWorld(**maybePart1)) return {nullptr, nullptr};
		auto part1 = std::dynamic_pointer_cast<BasePart>(*maybePart1);
		if (!part1) return {nullptr, nullptr};

		return {part0, part1};
	};

	bool Constraint::GetActive() const {
		if (!GetEnabled()) return false;

		auto [part0, part1] = GetActiveParts();
		const auto World = WorldRoot::GetEffectiveWorld(*this);
		return World && part0 && part1 && WorldRoot::GetEffectiveWorld(*part0) == World && WorldRoot::GetEffectiveWorld(*part1) == World;
	}
}
