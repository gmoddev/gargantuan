// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/generated/Terrain.hpp"

#include <memory>

namespace gargantuan {
	class Workspace;
	class WorldRoot;

	namespace terrain {
		class Field;
	}

	class Terrain final : public Instance {
		I_Terrain;

		~Terrain() override;
		[[nodiscard]] const terrain::Field &GetField() const;

	  private:
		friend class Instance;
		friend class Workspace;
		friend class WorldRoot;

		Terrain();
		void RetireField() noexcept;

		std::unique_ptr<terrain::Field> Field;
	};
}
