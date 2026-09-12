#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t Bodies(const WorldRoot &World) { return World.PartBodies.size(); }
		static std::size_t Constraints(const WorldRoot &World) { return World.ConstraintJoints.size(); }
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	double Milliseconds(Clock::time_point Started) {
		return std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
	}
	double Percentile(std::vector<double> Values, double Fraction) {
		std::ranges::sort(Values);
		return Values.empty() ? 0.0 : Values[static_cast<std::size_t>((Values.size() - 1) * Fraction)];
	}
	void Run(std::string_view Mode, std::size_t Count, int Repetition, int Steps) {
		auto World = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		if (!WorkspaceValue) throw std::runtime_error("Workspace fixture unavailable");
		WorkspaceValue->SetGravity(0.0f);
		const bool NonSpatial = Mode == "non-spatial";
		const bool Sensor = Mode == "anchored-touch-sensors";
		const bool DisabledTouch = Mode == "anchored-disabled-touch";
		const bool Assembly = Mode == "assembly-heavy";
		const bool Representative = Mode == "representative-rigid";
		std::shared_ptr<Part> Ground;
		if (Representative) {
			WorkspaceValue->SetGravity(196.2f);
			Ground = std::make_shared<Part>();
			Ground->SetAnchored(true);
			Ground->SetSize({256.0f, 2.0f, 256.0f});
			Ground->SetCFrame(CFrame(0.0f, -1.0f, 0.0f));
			Ground->SetParent(WorkspaceValue);
		}
		WorkspaceValue->StepPhysics(0.0, std::nullopt);
		const auto PrepareStarted = Clock::now();
		auto Group = std::make_shared<Folder>();
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Count);
		constexpr std::array Shapes{Enums::PartType::Block, Enums::PartType::Ball, Enums::PartType::Cylinder,
			Enums::PartType::Wedge, Enums::PartType::CornerWedge};
		for (std::size_t Index = 0; Index < Count; ++Index) {
			if (NonSpatial) { std::make_shared<Folder>()->SetParent(Group); continue; }
			auto Value = std::make_shared<Part>();
			Value->SetAnchored(Sensor || DisabledTouch || (Assembly && Index == 0));
			Value->SetCanCollide(!Sensor && !DisabledTouch);
			Value->SetCanTouch(!DisabledTouch);
			Value->SetSize({2.0f, 1.0f, 3.0f});
			if (Representative) {
				Value->SetShape(Shapes[Index % Shapes.size()]);
				Value->SetSize({2.0f, 2.0f, 2.0f});
				Value->SetCFrame(CFrame(static_cast<float>(Index % 8) * 2.1f,
					2.0f + static_cast<float>(Index / 64) * 2.1f, static_cast<float>((Index / 8) % 8) * 2.1f));
			} else {
				const float Spacing = Sensor || DisabledTouch ? 1.0f : 4.0f;
				Value->SetCFrame(CFrame(static_cast<float>(Index % 32) * Spacing, 2.0f,
					static_cast<float>(Index / 32) * Spacing));
			}
			Value->SetParent(Group);
			Parts.push_back(std::move(Value));
		}
		if (Assembly) {
			for (std::size_t Index = 1; Index < Parts.size(); ++Index) {
				auto Weld = std::make_shared<WeldConstraint>();
				Weld->SetPart0(Parts[Index - 1]);
				Weld->SetPart1(Parts[Index]);
				Weld->SetParent(Group);
			}
		}
		const auto PrepareMs = Milliseconds(PrepareStarted);
		const auto AttachStarted = Clock::now();
		Group->SetParent(WorkspaceValue); // One coherent authoritative Main-domain attachment.
		const auto AttachMs = Milliseconds(AttachStarted);
		const auto PreStepStarted = Clock::now();
		WorkspaceValue->StepPhysics(0.0, std::nullopt);
		const auto PreStepMs = Milliseconds(PreStepStarted);
		const auto ExpectedBodies = (NonSpatial ? 0 : Count) + (Ground ? 1 : 0);
		const auto ExpectedConstraints = Assembly ? Count - 1 : 0;
		if (WorldRootTestAccess::Bodies(*WorkspaceValue) != ExpectedBodies ||
			WorldRootTestAccess::Constraints(*WorkspaceValue) != ExpectedConstraints)
			throw std::runtime_error("coherent physics activation lost a body or constraint");
		std::vector<double> TickMs, RigidMs;
		TickMs.reserve(Steps); RigidMs.reserve(Steps);
		double FirstTickMs = 0;
		std::uint64_t MaximumRigidCalls = 0;
		for (int Tick = 0; Tick <= Steps; ++Tick) {
			runtime_detail::WorkSample Sample{};
			const auto Started = Clock::now();
			{
				runtime_detail::WorkCapture Capture(&Sample);
				WorkspaceValue->StepPhysics(1.0 / 60.0, std::nullopt);
			}
			const auto Elapsed = Milliseconds(Started);
			const auto &Rigid = Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::PhysicsRigidStep)];
			MaximumRigidCalls = std::max(MaximumRigidCalls, Rigid.Calls);
			if (Tick == 0) FirstTickMs = Elapsed;
			else { TickMs.push_back(Elapsed); RigidMs.push_back(Rigid.Nanoseconds / 1e6); }
		}
		const auto RemoveStarted = Clock::now();
		Group->Destroy();
		const auto RemoveMs = Milliseconds(RemoveStarted);
		if (WorldRootTestAccess::Bodies(*WorkspaceValue) != (Ground ? 1U : 0U) ||
			WorldRootTestAccess::Constraints(*WorkspaceValue) != 0)
			throw std::runtime_error("coherent physics removal retains body or constraint");
		std::cout << "[Physics:Activation] mode=" << Mode << " parts=" << (NonSpatial ? 0 : Count)
			<< " contentObjects=" << Count + 1 + ExpectedConstraints << " repetition=" << Repetition
			<< " prepareMs=" << PrepareMs << " atomicAttachMs=" << AttachMs << " preStepMs=" << PreStepMs
			<< " firstTickMs=" << FirstTickMs << " p50Ms=" << Percentile(TickMs, .50)
			<< " p95Ms=" << Percentile(TickMs, .95) << " p99Ms=" << Percentile(TickMs, .99)
			<< " maxMs=" << Percentile(TickMs, 1.0) << " rigidP99Ms=" << Percentile(RigidMs, .99)
			<< " rigidMaxMs=" << Percentile(RigidMs, 1.0) << " maxRigidCalls=" << MaximumRigidCalls
			<< " bodies=" << ExpectedBodies << " constraints=" << ExpectedConstraints << " removeMs=" << RemoveMs << '\n';
		World->Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Quick = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--quick";
		constexpr std::array Modes{"non-spatial", "anchored-disabled-touch", "anchored-touch-sensors",
			"simple-rigid", "representative-rigid", "assembly-heavy"};
		for (const auto Mode : Modes)
			for (const auto Count : {64U, 128U, 256U, 384U, 512U}) {
				if (Quick && Count != 64) continue;
				for (int Repetition = 0; Repetition < (Quick ? 1 : 3); ++Repetition)
					Run(Mode, Count, Repetition, Quick ? 12 : 120);
			}
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << "[Physics:Activation] " << Failure.what() << '\n';
		return 1;
	}
}
