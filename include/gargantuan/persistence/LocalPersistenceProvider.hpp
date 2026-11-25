#pragma once

#include "gargantuan/persistence/PersistenceProvider.hpp"

#include <filesystem>
#include <memory>

namespace gargantuan {
	struct LocalPersistenceLimits final {
		static constexpr std::size_t RecordCeiling = 10'000;
		static constexpr std::size_t PayloadByteCeiling = 32 * 1024 * 1024;

		std::size_t MaximumRecords = RecordCeiling;
		std::size_t MaximumPayloadBytes = PayloadByteCeiling;
	};

	struct DurableLocalPersistenceConfiguration final {
		static constexpr std::size_t MaximumProfileIdBytes = 4096;

		// Trusted host inputs. The parent directory must already exist on a local filesystem.
		// The profile ID and limits are checked against those recorded on first creation.
		// The provider owns its SQLite journal and the reserved .initializing sibling.
		std::filesystem::path Path;
		std::string ProfileId;
		LocalPersistenceLimits Limits;
		bool CreateIfMissing = true;
	};

	[[nodiscard]] bool SupportsDurableLocalPersistence() noexcept;

	// A null provider means invalid native configuration or an unavailable build capability.
	// Durable file access happens on the first Execute, within its cancellation and deadline.
	[[nodiscard]] std::shared_ptr<IPersistenceProvider> MakeMemoryPersistenceProvider(LocalPersistenceLimits Limits = {});
	[[nodiscard]] std::shared_ptr<IPersistenceProvider> MakeDurableLocalPersistenceProvider(
		DurableLocalPersistenceConfiguration Configuration);
}
