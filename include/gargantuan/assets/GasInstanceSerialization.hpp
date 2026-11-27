// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/serialization/GasDocument.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"

#include <memory>

namespace gargantuan { class Instance; }

namespace gargantuan::InstanceSerialization {
	struct GasMaterializationResult {
		std::vector<std::shared_ptr<Instance>> Roots;
		std::vector<std::string> Errors;
		[[nodiscard]] bool Succeeded() const { return !Roots.empty() && Errors.empty(); }
	};

	// Call on Main with an active frozen schema. Roots and siblings retain source order.
	// The caller supplies the asset dependency closure from the existing catalog.
	[[nodiscard]] SerializationResult<Gas::Document> CaptureGasDocument(
		std::span<const std::shared_ptr<Instance>> Roots,
		Gas::DocumentKind Kind,
		std::optional<Gas::PlaceMetadata> Place = {},
		std::vector<Gas::Dependency> Dependencies = {});

	// Reconstructs detached model roots or an isolated place DataModel on Main with
	// an active frozen schema. The caller owns dependency admission, destination
	// mutation, and any later activation.
	[[nodiscard]] GasMaterializationResult MaterializeGasDocument(
		const Gas::Document &Document, bool PreserveSourceIds, const TerrainSceneAdmission &AdmitTerrain = {},
		const DomainTerrainAdmission &AdmitDomains = {});
}
