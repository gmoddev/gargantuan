#pragma once

#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/classes/generated/DataModel.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <cstdint>
#include <filesystem>

namespace gargantuan {
	class DataModel : public ServiceProvider {
		I_DataModel;

		const ServiceDefinitions &GetServiceDefinitions() const override;

		std::filesystem::path Root;
		BaseFilesystem *Filesystem = nullptr;
		TagIndex Tags;

		static constexpr std::uint64_t InitialProjectRevision = 1;
		[[nodiscard]] std::uint64_t GetAuthoritativeRevision() const { return AuthoritativeRevision; }
		void EnsureAuthoritativeRevisionAvailable() const;
		void AdvanceAuthoritativeRevision();
		void InitializeLoadedProjectRevision();

	  private:
		std::uint64_t AuthoritativeRevision = InitialProjectRevision;
	};
}
