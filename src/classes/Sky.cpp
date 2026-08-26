#include "gargantuan/classes/Sky.hpp"

#include "gargantuan/assets/AssetTypes.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	bool Sky::GetEnabled() const {
		return Enabled;
	}
	void Sky::SetEnabled(bool Value) {
		AssertCanMutate();
		ValidatePropertyMutation("Enabled", Value);
		if (Enabled == Value) return;
		Enabled = Value;
		NotifyPropertyCommitted("Enabled");
	}

	void Sky::SetFace(std::string &Destination, std::string Value, const char *PropertyName) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Environment:Sky] Sky faces require a strict Image asset reference");
		ValidatePropertyMutation(PropertyName, Value);
		if (Destination == Value) return;
		Destination = std::move(Value);
		NotifyPropertyCommitted(PropertyName);
	}

	std::string Sky::GetSkyboxPositiveX() const {
		return SkyboxPositiveX;
	}
	void Sky::SetSkyboxPositiveX(std::string Value) {
		SetFace(SkyboxPositiveX, std::move(Value), "SkyboxPositiveX");
	}
	std::string Sky::GetSkyboxNegativeX() const {
		return SkyboxNegativeX;
	}
	void Sky::SetSkyboxNegativeX(std::string Value) {
		SetFace(SkyboxNegativeX, std::move(Value), "SkyboxNegativeX");
	}
	std::string Sky::GetSkyboxPositiveY() const {
		return SkyboxPositiveY;
	}
	void Sky::SetSkyboxPositiveY(std::string Value) {
		SetFace(SkyboxPositiveY, std::move(Value), "SkyboxPositiveY");
	}
	std::string Sky::GetSkyboxNegativeY() const {
		return SkyboxNegativeY;
	}
	void Sky::SetSkyboxNegativeY(std::string Value) {
		SetFace(SkyboxNegativeY, std::move(Value), "SkyboxNegativeY");
	}
	std::string Sky::GetSkyboxPositiveZ() const {
		return SkyboxPositiveZ;
	}
	void Sky::SetSkyboxPositiveZ(std::string Value) {
		SetFace(SkyboxPositiveZ, std::move(Value), "SkyboxPositiveZ");
	}
	std::string Sky::GetSkyboxNegativeZ() const {
		return SkyboxNegativeZ;
	}
	void Sky::SetSkyboxNegativeZ(std::string Value) {
		SetFace(SkyboxNegativeZ, std::move(Value), "SkyboxNegativeZ");
	}
}
