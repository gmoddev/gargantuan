#pragma once
#include "gargantuan/runtime/SessionSnapshot.hpp"
#include "gargantuan/services/generated/SessionService.hpp"

namespace gargantuan {
	class SessionService : public Instance {
		I_SessionService;
	};
}
