#pragma once

#include "gargantuan/content/ContentAvailability.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace gargantuan::host {
	struct NodeContentProviderConfiguration final {
		static constexpr std::size_t MaximumEndpointBytes = 512;
		static constexpr std::size_t MaximumCertificatePathBytes = 1'024;
		static constexpr std::size_t MaximumSecretReferenceBytes = 128;
		static constexpr std::size_t MaximumRootCertificateBytes = 1024 * 1024;
		static constexpr std::size_t MaximumWorkloadTokenBytes = 4'096;

		std::string Endpoint;
		std::filesystem::path RootCertificateFile;
		std::string WorkloadTokenEnvironment;
		std::size_t MaximumManifestResponseBytes = 4 * 1024 * 1024;
		std::size_t MaximumContentResponseBytes = MaximumPackageContentPayloadBytes;
		std::chrono::milliseconds ConnectionDeadline{2'000};
		std::chrono::milliseconds RequestDeadline{10'000};
	};

	class NodeContentProvider final : public IContentAvailabilityProvider {
	  public:
		struct Impl;
		explicit NodeContentProvider(NodeContentProviderConfiguration Configuration);
		~NodeContentProvider() override;

		[[nodiscard]] std::string_view Name() const override;
		[[nodiscard]] ContentProviderLifecycleResult Start(const ContentRequestContext &Context) override;
		void Stop() override;
		[[nodiscard]] ContentManifestProviderResult
		GetManifest(const ContentRequestContext &Context, const PackageContentNamespace &Package) override;
		[[nodiscard]] ContentPayloadProviderResult
		GetContent(const ContentRequestContext &Context, const PackageContentIdentity &Identity) override;

	  private:
		std::unique_ptr<Impl> State;
	};
} // namespace gargantuan::host
