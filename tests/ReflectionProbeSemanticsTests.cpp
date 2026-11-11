// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
#include "gargantuan/environment/ReflectionProbeSemantics.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>

using namespace gargantuan;
namespace {
	int Failures = 0;
	void Check(bool Passed, const char *Message) {
		if (!Passed) { ++Failures; std::cerr << "[Render:ProbeReference] " << Message << '\n'; }
	}
	RenderReflectionProbeState MakeProbe(std::uint32_t Id) {
		RenderReflectionProbeState Probe;
		Probe.Object = {Id, 1};
		Probe.Source.FaceDimension = 16;
		for (std::size_t Face = 0; Face < 6; ++Face) Probe.Source.Faces[Face] = {{Face + 1, 1}, 1};
		return Probe;
	}
	void Near(const glm::vec3 &Actual, const glm::vec3 &Expected, const char *Message) {
		Check(glm::length(Actual - Expected) < 1.0e-4f, Message);
	}
}
int main() {
	auto Probe = MakeProbe(1);
	Check(IsValidReflectionProbe(Probe), "valid probe rejected");
	Check(ReflectionProbeInfluence(Probe, {0, 0, 0}) == 1.0f, "center must cover global IBL");
	Check(ReflectionProbeInfluence(Probe, {8, 0, 0}) == 0.0f, "inner fade must reach zero at boundary");
	Check(ReflectionProbeInfluence(Probe, {8.01f, 0, 0}) == 0.0f, "outside volume contributes");
	Check(std::abs(ReflectionProbeInfluence(Probe, {7.5f, 0, 0}) - 0.5f) < 1.0e-6f, "smooth boundary midpoint");
	Near(ParallaxCorrectReflection(Probe, {2, 0, 0}, {0, 0, 1}), {2, 0, 8}, "parallel-axis box exit");
	Near(ParallaxCorrectReflection(Probe, {2, 0, 0}, {-1, 0, 0}), {-8, 0, 0}, "negative box exit");
	Probe.CaptureOffset = {1, 2, 3};
	Near(ParallaxCorrectReflection(Probe, {2, 0, 0}, {0, 0, 1}), {1, -2, 5}, "capture offset correction");
	Probe.Rotation = glm::mat3(glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 1, 0)));
	Near(ParallaxCorrectReflection(Probe, Probe.Rotation * glm::vec3(2, 0, 0), Probe.Rotation * glm::vec3(0, 0, 1)),
		{1, -2, 5}, "rotated volume direction");
	Probe.Position = {1.0e6f, 0, 0};
	Near(ParallaxCorrectReflection(Probe, Probe.Position, Probe.Rotation * glm::vec3(0, 0, 1)),
		{-1, -2, 5}, "large-coordinate translation");
	Probe.HalfSize.x = std::numeric_limits<float>::infinity();
	Check(!IsValidReflectionProbe(Probe), "infinite bounds accepted");
	Probe = MakeProbe(1); Probe.Rotation[0][0] = -1;
	Check(!IsValidReflectionProbe(Probe), "reflected orientation accepted");
	Probe = MakeProbe(1); Probe.CaptureOffset.x = 8;
	Check(!IsValidReflectionProbe(Probe), "capture origin outside volume accepted");
	std::array Probes{MakeProbe(2), MakeProbe(1)};
	for (int Step = -900; Step <= 900; ++Step) {
		const glm::vec3 Point(Step * 0.01f, 0, 0);
		const auto Blend = SelectReflectionProbes(Probes, Point);
		Check(std::abs(Blend.GlobalWeight + Blend.Weights[0] + Blend.Weights[1] - 1.0f) < 1e-6f,
			"blend injects or loses unit environment energy");
		Check(Blend.GlobalWeight >= 0 && Blend.Weights[0] >= 0 && Blend.Weights[1] >= 0, "negative blend");
	}
	const auto Blend = SelectReflectionProbes(Probes, {0, 0, 0});
	Check(Blend.Indices[0] == 1 && Blend.GlobalWeight == 0 && Blend.Weights[0] == 0.5f,
		"equal-priority probes must normalize and use identity tie order");
	Probes[0].Priority = 1;
	const auto Override = SelectReflectionProbes(Probes, {0, 0, 0});
	Check(Override.Indices[0] == 0 && Override.Weights[0] == 1 && Override.Weights[1] == 0,
		"high-priority full influence must override lower priority");
	Check(SelectReflectionProbes({}, {0, 0, 0}).GlobalWeight == 1, "missing probes must use global environment");
	std::cout << "[Render:ProbeReference] Failures=" << Failures << '\n';
	return Failures ? 1 : 0;
}
