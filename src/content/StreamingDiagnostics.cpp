#include "gargantuan/content/StreamingDiagnostics.hpp"

namespace gargantuan {
	const char *GetStreamingDiagnosticStatusName(StreamingDiagnosticStatus Status) {
		switch (Status) {
		case StreamingDiagnosticStatus::Disabled: return "Disabled";
		case StreamingDiagnosticStatus::NotConfigured: return "NotConfigured";
		case StreamingDiagnosticStatus::AwaitingManifest: return "AwaitingManifest";
		case StreamingDiagnosticStatus::Preparing: return "Preparing";
		case StreamingDiagnosticStatus::Active: return "Active";
		case StreamingDiagnosticStatus::PreparationFailed: return "PreparationFailed";
		case StreamingDiagnosticStatus::Stopped: return "Stopped";
		}
		return "Disabled";
	}
}
