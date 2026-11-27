// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/Log.hpp"
#include "gargantuan/datatypes/Signal.hpp"

#include <exception>

namespace gargantuan::network::detail {
	inline constexpr char CommittedObserverFailureMessage[] = "Local structural replication observer failed after frame publication";

	// Native signals propagate observer exceptions. These callers have already committed their local state.
	[[nodiscard]] inline bool DispatchReplicaNotifications(ScopedSignalDeferral &Notifications, const char *Operation) {
		try {
			Notifications.Commit();
			return true;
		} catch (const std::exception &Error) {
			LOG_WARN(App, "[Network:Replica] Local observer failed during %s: %.160s", Operation, Error.what());
		} catch (...) {
			LOG_WARN(App, "[Network:Replica] Local observer failed during %s", Operation);
		}
		return false;
	}
}
