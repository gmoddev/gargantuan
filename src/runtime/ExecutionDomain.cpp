#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <stdexcept>
#include <string>

namespace gargantuan {
	namespace {
		thread_local ExecutionDomain CurrentDomain = ExecutionDomain::Main;
	}

	ExecutionDomainScope::ExecutionDomainScope(ExecutionDomain domain) : Previous(CurrentDomain) {
		CurrentDomain = domain;
	}

	ExecutionDomainScope::~ExecutionDomainScope() {
		CurrentDomain = Previous;
	}

	ExecutionDomain GetCurrentExecutionDomain() {
		return CurrentDomain;
	}

	std::string_view GetExecutionDomainName(ExecutionDomain domain) {
		switch (domain) {
		case ExecutionDomain::Main: return "Main";
		case ExecutionDomain::Worker: return "Worker";
		case ExecutionDomain::Render: return "Render";
		case ExecutionDomain::Simulation: return "Simulation";
		case ExecutionDomain::IO: return "IO";
		}
		return "Unknown";
	}

	void AssertAuthoritativeMutation(std::string_view operation) {
		if (CurrentDomain != ExecutionDomain::Main) {
			throw std::logic_error(
				std::string(operation) + " requires the Main authoritative execution domain; current domain is " +
				std::string(GetExecutionDomainName(CurrentDomain))
			);
		}
	}
}
