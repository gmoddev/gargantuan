#include "ActionMapBaseline.hpp"
#include "gargantuan/input/CompiledInputGraph.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/UserInputService.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
	thread_local bool TrackAllocations = false;
	thread_local std::size_t AllocationCount = 0;
	thread_local std::size_t AllocationBytes = 0;
	void *Allocate(std::size_t Size) {
		if (TrackAllocations) {
			++AllocationCount;
			AllocationBytes += Size;
		}
		if (auto *Value = std::malloc(Size ? Size : 1)) return Value;
		throw std::bad_alloc();
	}
}

void *operator new(std::size_t Size) {
	return Allocate(Size);
}
void *operator new[](std::size_t Size) {
	return Allocate(Size);
}
void operator delete(void *Value) noexcept {
	std::free(Value);
}
void operator delete[](void *Value) noexcept {
	std::free(Value);
}
void operator delete(void *Value, std::size_t) noexcept {
	std::free(Value);
}
void operator delete[](void *Value, std::size_t) noexcept {
	std::free(Value);
}

namespace gargantuan {
	struct ActionMapBaselineTestAccess {
		static void Attach(ActionMapBaseline &Map, const std::shared_ptr<UserInputService> &Service) {
			Map.AttachInputService(Service);
		}
		static bool Process(ActionMapBaseline &Map, const HostEvent &Event) {
			return Map.ProcessEvent(Event);
		}
		static std::size_t MemoryBytes(const ActionMapBaseline &Map) {
			// Capacity-based owned storage estimate, excluding allocator/node bookkeeping and shared signals.
			std::size_t Bytes = sizeof(Map) + Map.Bindings.capacity() * sizeof(ActionMapBaseline::Binding) +
								Map.States.bucket_count() * sizeof(void *) +
								Map.States.size() *
									(sizeof(std::pair<const std::string, ActionMapBaseline::ActionState>) +
									 sizeof(void *));
			for (const auto &Value : Map.Bindings)
				if (Value.ActionName.size() > 15) Bytes += Value.ActionName.capacity() + 1;
			return Bytes;
		}
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using Json = nlohmann::json;
	using Graph = CompiledInputGraph;

	Json Percentiles(std::vector<double> Values) {
		std::ranges::sort(Values);
		auto At = [&](double Fraction) {
			return Values[std::min(
				Values.size() - 1, static_cast<std::size_t>(std::ceil(Values.size() * Fraction)) - 1
			)];
		};
		return {{"P50", At(0.50)}, {"P95", At(0.95)}, {"P99", At(0.99)}};
	}

	HostEvent Key(LogicalKey Code, ButtonState State) {
		return KeyEvent{.Device = {1}, .Logical = Code, .State = State};
	}

	Json Run(std::size_t BindingCount, std::size_t ContextCount, bool Dense, bool Quick) {
		auto Physical = std::make_shared<UserInputService>();
		auto Baseline = std::make_shared<ActionMapBaseline>();
		ActionMapBaselineTestAccess::Attach(*Baseline, Physical);
		auto Compiled = std::make_unique<Graph>();
		std::vector<Graph::ContextDefinition> Contexts;
		std::vector<Graph::ActionDefinition> Actions;
		std::vector<Graph::BindingDefinition> Bindings;
		std::vector<int> LegacyIds;
		const auto ActionCount = std::min<std::size_t>(128, BindingCount);
		for (std::size_t Index = 0; Index < ContextCount; ++Index)
			Contexts.push_back(
				{.Id = {static_cast<std::uint32_t>(Index + 1), 1},
				 .Priority = static_cast<int>(Index),
				 .LegacyPassthrough = true}
			);
		for (std::size_t Index = 0; Index < ActionCount; ++Index)
			Actions.push_back(
				{.Id = {static_cast<std::uint32_t>(Index + 100), 1},
				 .Context = Contexts[Index % ContextCount].Id,
				 .ValueType = Graph::Type::Scalar}
			);
		for (std::size_t Index = 0; Index < BindingCount; ++Index) {
			const auto Code = Dense ? LogicalKey::W
									: static_cast<LogicalKey>(static_cast<int>(LogicalKey::A) + Index % 26);
			const auto ActionIndex = Index % ActionCount;
			LegacyIds.push_back(Baseline->BindKey(
				"Action" + std::to_string(ActionIndex), static_cast<Enums::KeyCode>(Code), 1.0f, 0, false
			));
			Bindings.push_back(
				{.Id = {static_cast<std::uint32_t>(Index + 1000), 1},
				 .Action = Actions[ActionIndex].Id,
				 .Code = static_cast<std::uint16_t>(Code)}
			);
		}
		const auto CompilationStart = Clock::now();
		(void)Compiled->Compile(Contexts, Actions, Bindings);
		const double InitialCompileUs =
			std::chrono::duration<double, std::micro>(Clock::now() - CompilationStart).count();
		const auto Count = Quick ? 2000u : 20000u;
		std::vector<double> LegacySamples, CompiledSamples;
		LegacySamples.reserve(Count);
		CompiledSamples.reserve(Count);
		std::size_t LegacyAllocations = 0, CompiledAllocations = 0, LegacyBytes = 0, CompiledBytes = 0;
		std::size_t Evaluated = 0;
		for (std::size_t Index = 0; Index < Count + 200; ++Index) {
			const auto Event = Key(LogicalKey::W, Index % 2 == 0 ? ButtonState::Pressed : ButtonState::Released);
			(void)Physical->ProcessEvent(Event);
			AllocationCount = AllocationBytes = 0;
			TrackAllocations = true;
			const auto LegacyStart = Clock::now();
			(void)ActionMapBaselineTestAccess::Process(*Baseline, Event);
			const auto LegacyEnd = Clock::now();
			TrackAllocations = false;
			if (Index >= 200) {
				LegacySamples.push_back(std::chrono::duration<double, std::nano>(LegacyEnd - LegacyStart).count());
				LegacyAllocations += AllocationCount;
				LegacyBytes += AllocationBytes;
			}
			AllocationCount = AllocationBytes = 0;
			TrackAllocations = true;
			const auto CompiledStart = Clock::now();
			(void)Compiled->ProcessEvent(Event);
			const auto CompiledEnd = Clock::now();
			TrackAllocations = false;
			if (Index >= 200) {
				CompiledSamples.push_back(
					std::chrono::duration<double, std::nano>(CompiledEnd - CompiledStart).count()
				);
				CompiledAllocations += AllocationCount;
				CompiledBytes += AllocationBytes;
				Evaluated += Compiled->GetStatistics().EvaluatedBindings;
			}
			if (Index == 200 || Index == 201) {
				for (std::size_t ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex) {
					const auto State = Compiled->GetState(Actions[ActionIndex].Id);
					const auto Name = "Action" + std::to_string(ActionIndex);
					if (State.Down != Baseline->IsDown(Name) || State.Value != Baseline->GetValue(Name))
						throw std::runtime_error("[Input:Benchmark] Baseline/compiled action state mismatch");
				}
			}
		}
		std::vector<double> EditSamples, LegacyEditSamples;
		std::size_t Reindexed = 0;
		for (std::size_t Index = 0; Index < (Quick ? 20u : 200u); ++Index) {
			const auto Code = Index % 2 == 0 ? LogicalKey::S : LogicalKey::W;
			Bindings.front().Code = static_cast<std::uint16_t>(Code);
			const auto Start = Clock::now();
			(void)Compiled->Compile(Contexts, Actions, Bindings);
			EditSamples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
			Reindexed += Compiled->GetStatistics().ReindexedBuckets;
			const auto LegacyStart = Clock::now();
			(void)Baseline->Unbind(LegacyIds.front());
			LegacyIds.front() = Baseline->BindKey("Action0", static_cast<Enums::KeyCode>(Code), 1.0f, 0, false);
			LegacyEditSamples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - LegacyStart).count());
		}
		return {
			{"Bindings", BindingCount},
			{"Actions", ActionCount},
			{"Contexts", ContextCount},
			{"Distribution", Dense ? "Dense" : "Spread"},
			{"Events", Count},
			{"InitialCompileUs", InitialCompileUs},
			{"CompileEditUs", Percentiles(EditSamples)},
			{"LegacyRemoveAddUs", Percentiles(LegacyEditSamples)},
			{"EditedBucketsTotal", Reindexed},
			{"Baseline",
			 {{"EventNs", Percentiles(LegacySamples)},
			  {"AllocationsPerEvent", static_cast<double>(LegacyAllocations) / Count},
			  {"AllocatedBytesPerEvent", static_cast<double>(LegacyBytes) / Count},
			  {"EstimatedStorageBytes", ActionMapBaselineTestAccess::MemoryBytes(*Baseline)}}},
			{"Compiled",
			 {{"EventNs", Percentiles(CompiledSamples)},
			  {"AllocationsPerEvent", static_cast<double>(CompiledAllocations) / Count},
			  {"AllocatedBytesPerEvent", static_cast<double>(CompiledBytes) / Count},
			  {"EstimatedStorageBytes", Compiled->GetStatistics().MemoryBytes},
			  {"CandidatesPerEvent", static_cast<double>(Evaluated) / Count}}},
		};
	}
}

int main(int Count, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Quick = Count > 1 && std::string_view(Arguments[1]) == "--quick";
		Json Rows = Json::array();
		for (const auto Bindings : {16u, 128u, 512u})
			for (const auto Contexts : {1u, 8u, 32u})
				for (const bool Dense : {false, true})
					Rows.push_back(Run(Bindings, Contexts, Dense, Quick));
		std::cout
			<< Json({
						{"BaselineSource", "eb2ce26ab:src/services/ActionMap.cpp"},
						{"Scope",
						 "Native evaluator only; UIS dispatch, Instance snapshot construction, and Luau subscribers "
						 "excluded. Baseline includes empty native signals; compiled transition dispatch excluded. "
						 "Legacy has no contexts: compiled partitions use passthrough semantics."},
						{"AllocationScope",
						 "Ordinary executable C++ new/new[] requests during event evaluation, after warmup; aligned "
						 "allocations excluded."},
						{"MemoryScope",
						 "Capacity-based storage estimates excluding allocator bookkeeping, temporary compiler "
						 "storage, and Instance graphs."},
						{"Rows", Rows},
					})
				   .dump(2)
			<< '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Input:Benchmark] " << Error.what() << '\n';
		return 1;
	}
}
