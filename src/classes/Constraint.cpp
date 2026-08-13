#include "gargantuan/classes/Constraint.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include <memory>

namespace gargantuan {
	std::tuple<std::shared_ptr<BasePart>, std::shared_ptr<BasePart>> Constraint::GetActiveParts() const {
		if (!Attachment0 || !*Attachment0 || !Attachment1 || !*Attachment1) return {nullptr, nullptr};
		auto maybePart0 = (*Attachment0)->GetParent();
		if (!maybePart0 || !(*maybePart0)->FindFirstAncestorWhichIsA("WorldRoot")) return {nullptr, nullptr};
		auto part0 = std::dynamic_pointer_cast<BasePart>(*maybePart0);
		if (!part0) return {nullptr, nullptr};

		auto maybePart1 = (*Attachment1)->GetParent();
		if (!maybePart1 || !(*maybePart1)->FindFirstAncestorWhichIsA("WorldRoot")) return {nullptr, nullptr};
		auto part1 = std::dynamic_pointer_cast<BasePart>(*maybePart1);
		if (!part1) return {nullptr, nullptr};

		return {part0, part1};
	};

	bool Constraint::GetActive() const {
		if (!GetEnabled() || !Attachment0.has_value() || !Attachment1.has_value()) return false;

		auto [part0, part1] = GetActiveParts();
		return part0 && part1;
	}
}
