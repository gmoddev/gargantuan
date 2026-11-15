#include "gargantuan/classes/ReflectionProbe.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/render/RenderEnvironment.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		bool InRange(float Value, float Minimum, float Maximum) {
			return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
		}
		bool Finite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}
	}
	void ReflectionProbe::SetFace(std::string &Destination, std::string Value, const char *PropertyName) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Environment:ReflectionProbe] Expected an Image asset reference");
		ValidatePropertyMutation(PropertyName, Value);
		if (Destination == Value) return;
		Destination = std::move(Value);
		NotifyPropertyCommitted(PropertyName);
	}
	CFrame ReflectionProbe::GetProbeCFrame() const { return ProbeCFrame; }
	void ReflectionProbe::SetProbeCFrame(CFrame Value) {
		AssertCanMutate();
		bool Valid = Finite(Value.Position) && glm::all(glm::lessThanEqual(glm::abs(Value.Position), glm::vec3(1.0e6f)));
		for (int Column = 0; Column < 3; ++Column) {
			Valid = Valid && Finite(Value.Rotation[Column]);
			for (int Other = Column; Other < 3; ++Other)
				Valid = Valid && std::abs(glm::dot(Value.Rotation[Column], Value.Rotation[Other]) - (Column == Other ? 1.0f : 0.0f)) <= 1.0e-3f;
		}
		Valid = Valid && std::abs(glm::determinant(Value.Rotation) - 1.0f) <= 1.0e-3f;
		if (!Valid) throw std::invalid_argument("[Environment:ReflectionProbe] Invalid ProbeCFrame");
		ValidatePropertyMutation("ProbeCFrame", Value);
		if (ProbeCFrame.Position == Value.Position && ProbeCFrame.Rotation == Value.Rotation) return;
		ProbeCFrame = Value;
		NotifyPropertyCommitted("ProbeCFrame");
	}
	glm::vec3 ReflectionProbe::GetProbeSize() const { return ProbeSize; }
	void ReflectionProbe::SetProbeSize(glm::vec3 Value) {
		AssertCanMutate();
		if (!Finite(Value) || glm::any(glm::lessThan(Value, glm::vec3(0.1f))) || glm::any(glm::greaterThan(Value, glm::vec3(16384.0f))) || glm::any(glm::greaterThanEqual(glm::abs(ProbeCaptureOffset), Value * 0.5f))) throw std::invalid_argument("[Environment:ReflectionProbe] Invalid ProbeSize");
		ValidatePropertyMutation("ProbeSize", Value);
		if (ProbeSize == Value) return;
		ProbeSize = Value;
		NotifyPropertyCommitted("ProbeSize");
	}
	glm::vec3 ReflectionProbe::GetProbeCaptureOffset() const { return ProbeCaptureOffset; }
	void ReflectionProbe::SetProbeCaptureOffset(glm::vec3 Value) {
		AssertCanMutate();
		if (!Finite(Value) || glm::any(glm::greaterThanEqual(glm::abs(Value), ProbeSize * 0.5f))) throw std::invalid_argument("[Environment:ReflectionProbe] Invalid ProbeCaptureOffset");
		ValidatePropertyMutation("ProbeCaptureOffset", Value);
		if (ProbeCaptureOffset == Value) return;
		ProbeCaptureOffset = Value;
		NotifyPropertyCommitted("ProbeCaptureOffset");
	}
	float ReflectionProbe::GetProbeBlendDistance() const { return ProbeBlendDistance; }
	void ReflectionProbe::SetProbeBlendDistance(float Value) {
		AssertCanMutate();
		if (!InRange(Value, 0.0f, 8192.0f)) throw std::invalid_argument("[Environment:ReflectionProbe] Invalid ProbeBlendDistance");
		ValidatePropertyMutation("ProbeBlendDistance", Value);
		if (ProbeBlendDistance == Value) return;
		ProbeBlendDistance = Value;
		NotifyPropertyCommitted("ProbeBlendDistance");
	}
	float ReflectionProbe::GetProbeIntensity() const { return ProbeIntensity; }
	void ReflectionProbe::SetProbeIntensity(float Value) {
		AssertCanMutate();
		if (!InRange(Value, 0.0f, 8.0f)) throw std::invalid_argument("[Environment:ReflectionProbe] Invalid ProbeIntensity");
		ValidatePropertyMutation("ProbeIntensity", Value);
		if (ProbeIntensity == Value) return;
		ProbeIntensity = Value;
		NotifyPropertyCommitted("ProbeIntensity");
	}
	int ReflectionProbe::GetProbePriority() const { return ProbePriority; }
	void ReflectionProbe::SetProbePriority(int Value) {
		AssertCanMutate();
		if (Value < -1024 || Value > 1024) throw std::invalid_argument("[Environment:ReflectionProbe] Invalid ProbePriority");
		ValidatePropertyMutation("ProbePriority", Value);
		if (ProbePriority == Value) return;
		ProbePriority = Value;
		NotifyPropertyCommitted("ProbePriority");
	}
	std::string ReflectionProbe::GetProbePositiveX() const { return ProbePositiveX; }
	void ReflectionProbe::SetProbePositiveX(std::string Value) {
		SetFace(ProbePositiveX, std::move(Value), "ProbePositiveX");
	}
	std::string ReflectionProbe::GetProbeNegativeX() const { return ProbeNegativeX; }
	void ReflectionProbe::SetProbeNegativeX(std::string Value) {
		SetFace(ProbeNegativeX, std::move(Value), "ProbeNegativeX");
	}
	std::string ReflectionProbe::GetProbePositiveY() const { return ProbePositiveY; }
	void ReflectionProbe::SetProbePositiveY(std::string Value) {
		SetFace(ProbePositiveY, std::move(Value), "ProbePositiveY");
	}
	std::string ReflectionProbe::GetProbeNegativeY() const { return ProbeNegativeY; }
	void ReflectionProbe::SetProbeNegativeY(std::string Value) {
		SetFace(ProbeNegativeY, std::move(Value), "ProbeNegativeY");
	}
	std::string ReflectionProbe::GetProbePositiveZ() const { return ProbePositiveZ; }
	void ReflectionProbe::SetProbePositiveZ(std::string Value) {
		SetFace(ProbePositiveZ, std::move(Value), "ProbePositiveZ");
	}
	std::string ReflectionProbe::GetProbeNegativeZ() const { return ProbeNegativeZ; }
	void ReflectionProbe::SetProbeNegativeZ(std::string Value) {
		SetFace(ProbeNegativeZ, std::move(Value), "ProbeNegativeZ");
	}
}
