#pragma once

#include "gargantuan/persistence/PersistenceProvider.hpp"

#include <memory>

namespace gargantuan {
	class DataModel;
	class Script;
	class PersistenceRuntime;
	struct PersistenceResultBudget;

	// Native Main-domain handle. A worker never publishes directly to a handle.
	class PersistenceHandle final {
	public:
		PersistenceHandle() = default;
		PersistenceHandle(const PersistenceHandle&) = delete;
		PersistenceHandle& operator=(const PersistenceHandle&) = delete;
		const PersistenceResult& Poll() const noexcept { return Result; }
		bool Cancel();
		void ConsumePayload();
		~PersistenceHandle();

	private:
		friend class PersistenceRuntime;
		void ReleaseReservation() noexcept;
		PersistenceResult Result{.Status = PersistenceStatus::Pending};
		std::stop_source Stop;
		std::shared_ptr<PersistenceResultBudget> Budget;
		std::size_t ReservedBytes = 0;
	};

	class PersistenceRuntime final {
	public:
		static constexpr std::size_t MaximumInFlight = 32;
		static constexpr std::size_t MaximumPerOwner = 8;
		static constexpr std::size_t MaximumUnattributed = 8;
		static constexpr std::size_t MaximumCompletionsPerStep = 8;
		static constexpr std::size_t MaximumReservedResultBytes = 16 * 1024 * 1024;
		static constexpr auto DefaultDeadline = std::chrono::milliseconds(1000);
		static constexpr auto MaximumDeadline = std::chrono::milliseconds(2000);

		PersistenceRuntime(std::shared_ptr<IPersistenceProvider> Provider, std::weak_ptr<DataModel> World);
		~PersistenceRuntime();
		PersistenceRuntime(const PersistenceRuntime&) = delete;
		PersistenceRuntime& operator=(const PersistenceRuntime&) = delete;
		PersistenceCapabilities Capabilities() const;
		std::shared_ptr<PersistenceHandle> Submit(PersistenceRequest Request, std::weak_ptr<Script> Owner = {},
			std::chrono::milliseconds Timeout = DefaultDeadline);
		void Pump();
		void Shutdown();

	private:
		struct State;
		std::unique_ptr<State> Runtime;
	};
}
