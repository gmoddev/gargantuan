#include "gargantuan/network/Connection.hpp"

namespace gargantuan::network {
	bool IsLegalConnectionTransition(ConnectionState From, ConnectionState To) {
		if (From == To || From == ConnectionState::Closed) return false;
		switch (From) {
		case ConnectionState::Connecting:
			return To == ConnectionState::Authenticating || To == ConnectionState::Closing ||
				To == ConnectionState::Closed;
		case ConnectionState::Authenticating:
			return To == ConnectionState::Connected || To == ConnectionState::Closing ||
				To == ConnectionState::Closed;
		case ConnectionState::Connected:
			return To == ConnectionState::Closing || To == ConnectionState::Closed;
		case ConnectionState::Closing:
			return To == ConnectionState::Closed;
		case ConnectionState::Closed:
			return false;
		}
		return false;
	}

	bool IsTerminalConnectionState(ConnectionState State) { return State == ConnectionState::Closed; }
}
