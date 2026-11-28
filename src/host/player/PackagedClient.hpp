// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "StandalonePlayer.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/TerrainSession.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"

namespace gargantuan::host {
	inline network::GameSessionConfiguration ConfigurePackagedClient(const RuntimePackagePayload &Payload,
		const std::shared_ptr<DataModel> &TrustedWorld, const network::TransportEndpoint &Endpoint,
		bool NetworkTerrain, std::uint64_t Nonce = 0, bool AllowDevelopmentNetwork = false,
		const std::optional<PlayerAdmission> &Admission = {}) {
		return {
			.Role = network::GameSessionRole::Client, .Endpoint = Endpoint,
			.Limits = network::GameSessionConfiguration::DefaultLimits(), .ClientNonce = Nonce,
			.AllowInsecureDevelopmentNetwork = AllowDevelopmentNetwork,
			.AdmissionMode = Admission ? network::GameSessionAdmissionMode::Authenticated : network::GameSessionAdmissionMode::DevelopmentLocal,
			.AdmissionProof = Admission ? std::optional(Admission->Proof) : std::nullopt,
			.ExpectedServerPublicKey = Admission ? Admission->ServerKey : std::nullopt, .DeclareNormalOrdinaryService = true,
			.HydratePlayerGuiCode = Payload.ClientCodeJson ? PackageBuilder::MakeDistributedPlayerGuiCodeHydrator(Payload)
				: PackageBuilder::MakePlayerGuiCodeHydrator(TrustedWorld),
			.TerrainBootstrap = !Payload.WorkspaceTerrainManifest.empty() ?
				network::MakeTerrainReceiver(Payload.Inspection.Identity, Payload.Inspection.Revision,
					*AssetContentId::Parse(Payload.WorkspaceTerrainManifest),
					terrain::GridSpec{static_cast<std::int8_t>(Payload.WorkspaceTerrainSpacingShift)}) :
				NetworkTerrain ? network::MakeTerrainSessionData(Payload.Inspection.Identity, Payload.Inspection.Revision, Payload.TerrainArtifacts, false) : nullptr,
			.RuntimeDistributionIdentity = Payload.ClientCodeIdentity,
			.HydratePackagedCode = PackageBuilder::MakeDistributedCodeHydrator(Payload), .SpatialContent = Payload.SpatialContent,
		};
	}
}
