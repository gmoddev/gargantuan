#pragma once

#include <string_view>

namespace gargantuan {
	enum class ExecutionDomain { Main, Worker, Render, Simulation, IO };

	class ExecutionDomainScope {
	  public:
		explicit ExecutionDomainScope(ExecutionDomain domain);
		~ExecutionDomainScope();

		ExecutionDomainScope(const ExecutionDomainScope &) = delete;
		ExecutionDomainScope &operator=(const ExecutionDomainScope &) = delete;

	  private:
		ExecutionDomain Previous;
	};

	[[nodiscard]] ExecutionDomain GetCurrentExecutionDomain();
	[[nodiscard]] std::string_view GetExecutionDomainName(ExecutionDomain domain);
	void AssertAuthoritativeMutation(std::string_view operation);
}
