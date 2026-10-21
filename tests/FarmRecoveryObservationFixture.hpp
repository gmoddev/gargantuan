#pragma once

#include "../src/host/player/FarmRecoveryObservation.hpp"
#include "gargantuan/classes/Folder.hpp"

#include <iostream>
#include <array>
#include <utility>

namespace gargantuan::test {

inline bool RunFarmRecoveryObservationTests() {
	auto World = std::make_shared<DataModel>();
	auto Unrelated = std::make_shared<Folder>();
	Unrelated->SetParent(World);
	bool Passed = host::detail::FarmRecoveryCaseIndex(*World) == 0;
	// A value on another service/object cannot supply the DataModel stage.
	Passed &= Unrelated->ApplyAttributeMutation("ScaleOverloadCase", WireValue(std::string("recover_mixed")),
		ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success;
	Passed &= host::detail::FarmRecoveryCaseIndex(*World) == 0;
	for (const auto &[Stage, Expected] : std::array<std::pair<std::string_view, std::size_t>, 7>{
		{{"gameplay", 0}, {"recover_gameplay", 0}, {"structural", 0}, {"recover_structural", 1},
		 {"mixed", 0}, {"recover_mixed", 2}, {"complete", 0}}}) {
		Passed &= World->ApplyAttributeMutation("ScaleOverloadCase", WireValue(std::string(Stage)),
			ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success;
		Passed &= host::detail::FarmRecoveryCaseIndex(*World) == Expected;
	}
	Passed &= World->ApplyAttributeMutation("ScaleOverloadCase", WireValue(2),
		ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success;
	Passed &= host::detail::FarmRecoveryCaseIndex(*World) == 0;
	World->Destroy();
	std::cout << "[Qualification:RecoveryObservation] DataModel_stage_cases=10 result=" << (Passed ? "pass" : "fail") << '\n';
	return Passed;
}

} // namespace gargantuan::test
