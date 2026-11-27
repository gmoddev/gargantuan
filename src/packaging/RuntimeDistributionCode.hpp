#pragma once

#include "gargantuan/packaging/RuntimeDistribution.hpp"
#include "gargantuan/packaging/RuntimePackageCrypto.hpp"
#include "serialization/JsonCodec.hpp"

#include <functional>
#include <map>

namespace gargantuan {
	class DataModel;
	class RuntimeScriptCode;

	struct RuntimeDistributionArtifact final {
		std::string SourceId;
		std::string Class;
		std::vector<std::string> Path;
		RuntimeDistributionRepresentation Representation = RuntimeDistributionRepresentation::Source;
		RuntimeDistributionDiagnostics Diagnostics = RuntimeDistributionDiagnostics::Minimal;
		std::shared_ptr<const RuntimeScriptCode> Code;
	};

	struct RuntimeDistributionCodeCatalog final {
		std::map<std::string, RuntimeDistributionArtifact, std::less<>> Scripts;
	};

	namespace runtime_distribution {
		inline constexpr std::string_view CodeCatalogPath = "content/runtime-scripts.json";
		inline constexpr std::string_view SignaturePath = "game.package.sig";
		inline constexpr std::size_t MaximumCatalogBytes = 4 * 1024 * 1024;

		struct Metadata final {
			RuntimeDistributionTarget Target = RuntimeDistributionTarget::OfflinePlayer;
			RuntimeDistributionKeyAccess KeyAccess = RuntimeDistributionKeyAccess::None;
			std::string ReleaseId;
			std::string PublisherId;
			std::string SigningKeyId;
			std::string ContentKeyId;
			std::string Compiler;
			std::string ClientCodeIdentity;
			bool SpatialContent{};
		};

		using ArtifactWriter = std::function<void(std::string, std::string_view)>;
		using ArtifactReader = std::function<std::string(std::string_view, std::size_t)>;

		[[nodiscard]] JsonCodec::Json ParseStrictJson(std::string_view Text, std::size_t MaximumBytes);
		[[nodiscard]] JsonCodec::Json EncodeMetadata(const Metadata &Value);
		[[nodiscard]] Metadata DecodeMetadata(const JsonCodec::Json &Value);
		[[nodiscard]] std::string RemoveCompiledSource(
			std::string_view Snapshot, const RuntimeDistributionPlan &Plan, bool Distributed
		);
		[[nodiscard]] std::string BuildCatalog(
			const RuntimeDistributionPlan &Plan, const Metadata &Value, std::string_view Project,
			const RuntimePackageProvisionedSecurity &Security, const ArtifactWriter &Write
		);
		[[nodiscard]] RuntimeDistributionCodeCatalog DecodeCatalog(std::string_view Catalog);
		void AdmitCatalog(
			RuntimeDistributionCodeCatalog &Catalog, const Metadata &Value, std::string_view Project,
			const RuntimePackageProvisionedSecurity &Security, bool TrustedCompiler, const ArtifactReader &Read
		);
		[[nodiscard]] std::size_t BindCatalog(
			const RuntimeDistributionCodeCatalog &Catalog, const std::shared_ptr<DataModel> &World, bool Distributed
		);
	}
}
