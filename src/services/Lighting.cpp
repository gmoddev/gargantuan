#include "gargantuan/services/Lighting.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	namespace {
		bool Equal(const Color3 &Left, const Color3 &Right) {
			return Left.R == Right.R && Left.G == Right.G && Left.B == Right.B;
		}
	}

	Color3 Lighting::GetAmbient() const {
		return Ambient;
	}
	void Lighting::SetAmbient(Color3 Value) {
		AssertCanMutate();
		ValidatePropertyMutation("Ambient", Value);
		if (Equal(Ambient, Value)) return;
		Ambient = Value;
		NotifyPropertyCommitted("Ambient");
	}

	Color3 Lighting::GetSunColor() const {
		return SunColor;
	}
	void Lighting::SetSunColor(Color3 Value) {
		AssertCanMutate();
		ValidatePropertyMutation("SunColor", Value);
		if (Equal(SunColor, Value)) return;
		SunColor = Value;
		NotifyPropertyCommitted("SunColor");
	}

	float Lighting::GetBrightness() const {
		return Brightness;
	}
	void Lighting::SetBrightness(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("Brightness", Value);
		if (Brightness == Value) return;
		Brightness = Value;
		NotifyPropertyCommitted("Brightness");
	}

	float Lighting::GetClockTime() const {
		return ClockTime;
	}
	void Lighting::SetClockTime(float Value) {
		AssertCanMutate();
		if (!std::isfinite(Value) || Value < 0.0f || Value >= 24.0f)
			throw std::invalid_argument("[Environment:Lighting] ClockTime must satisfy 0 <= value < 24");
		ValidatePropertyMutation("ClockTime", Value);
		if (ClockTime == Value) return;
		ClockTime = Value;
		NotifyPropertyCommitted("ClockTime");
	}

	float Lighting::GetExposureCompensation() const {
		return ExposureCompensation;
	}
	void Lighting::SetExposureCompensation(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("ExposureCompensation", Value);
		if (ExposureCompensation == Value) return;
		ExposureCompensation = Value;
		NotifyPropertyCommitted("ExposureCompensation");
	}

	Color3 Lighting::GetEnvironmentColor() const {
		return EnvironmentColor;
	}
	void Lighting::SetEnvironmentColor(Color3 Value) {
		AssertCanMutate();
		ValidatePropertyMutation("EnvironmentColor", Value);
		if (Equal(EnvironmentColor, Value)) return;
		EnvironmentColor = Value;
		NotifyPropertyCommitted("EnvironmentColor");
	}

	bool Lighting::GetFogEnabled() const {
		return FogEnabled;
	}
	void Lighting::SetFogEnabled(bool Value) {
		AssertCanMutate();
		ValidatePropertyMutation("FogEnabled", Value);
		if (FogEnabled == Value) return;
		FogEnabled = Value;
		NotifyPropertyCommitted("FogEnabled");
	}

	Color3 Lighting::GetFogColor() const {
		return FogColor;
	}
	void Lighting::SetFogColor(Color3 Value) {
		AssertCanMutate();
		ValidatePropertyMutation("FogColor", Value);
		if (Equal(FogColor, Value)) return;
		FogColor = Value;
		NotifyPropertyCommitted("FogColor");
	}

	float Lighting::GetFogStart() const {
		return FogStart;
	}
	void Lighting::SetFogStart(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("FogStart", Value);
		if (Value > FogEnd)
			throw std::invalid_argument("[Environment:Fog] FogStart must be less than or equal to FogEnd");
		if (FogStart == Value) return;
		FogStart = Value;
		NotifyPropertyCommitted("FogStart");
	}

	float Lighting::GetFogEnd() const {
		return FogEnd;
	}
	void Lighting::SetFogEnd(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("FogEnd", Value);
		if (Value < FogStart)
			throw std::invalid_argument("[Environment:Fog] FogEnd must be greater than or equal to FogStart");
		if (FogEnd == Value) return;
		FogEnd = Value;
		NotifyPropertyCommitted("FogEnd");
	}
}
