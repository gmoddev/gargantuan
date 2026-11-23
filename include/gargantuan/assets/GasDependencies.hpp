// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/serialization/GasDocument.hpp"

namespace gargantuan { class AssetService; }

namespace gargantuan::Gas {
	// Resolves authored references, explicitly supplied pins, and transitive
	// dependencies against the existing project catalog. Performs no mutation.
	[[nodiscard]] SerializationResult<std::vector<Dependency>> ResolveDependencies(
		const Document &DocumentValue, const AssetService *Assets, bool IncludeEntireCatalog);
}
