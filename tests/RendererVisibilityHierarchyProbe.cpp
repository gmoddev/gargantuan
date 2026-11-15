// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Standalone renderer-owned visibility experiment. No Engine Instances, SDL,
// GPU, or authoritative spatial index participate in this measurement.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "gargantuan/render/RenderPublication.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using ClipPlanes = std::array<glm::vec4, 6>;
	constexpr RenderBounds PrimitiveBounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
	constexpr std::uint32_t LeafSize = 32;
	std::atomic_uint64_t Sink{};

	// Verbatim Foundation 4 OpaquePass camera planes and object-box rejection.
	ClipPlanes GetClipPlanes(const RenderCameraSnapshot &Camera) {
		const auto Rows = glm::transpose(Camera.ProjectionMatrix * Camera.ViewMatrix);
		return {Rows[3] + Rows[0], Rows[3] - Rows[0], Rows[3] + Rows[1], Rows[3] - Rows[1],
			Rows[2], Rows[3] - Rows[2]};
	}

	bool OutsideView(const ClipPlanes &Planes, const glm::mat4 &Model, const RenderBounds &Bounds) {
		const auto Center = Bounds.Minimum * 0.5f + Bounds.Maximum * 0.5f;
		const auto Extent = Bounds.Maximum * 0.5f - Bounds.Minimum * 0.5f;
		const auto ToLocal = glm::transpose(Model);
		for (const auto &Plane : Planes) {
			const auto LocalPlane = ToLocal * Plane;
			const auto Normal = glm::vec3(LocalPlane);
			const auto AbsoluteNormal = glm::abs(Normal);
			const auto Radius = glm::dot(AbsoluteNormal, Extent);
			const auto Distance = glm::dot(Normal, Center) + LocalPlane.w;
			const auto Margin = 1e-5f * (1.0f + glm::dot(AbsoluteNormal, glm::abs(Center)) +
				std::abs(LocalPlane.w) + Radius);
			if (std::isfinite(Distance) && std::isfinite(Radius) && Distance + Radius < -Margin)
				return true;
		}
		return false;
	}

	struct WorldBox {
		glm::dvec3 Minimum{std::numeric_limits<double>::infinity()};
		glm::dvec3 Maximum{-std::numeric_limits<double>::infinity()};
		bool Valid = false;
		bool Unbounded = false;
	};

	void Include(WorldBox &Destination, const WorldBox &Source) {
		if (Destination.Unbounded) return;
		if (Source.Unbounded) { Destination.Valid = false; Destination.Unbounded = true; return; }
		if (!Source.Valid) return;
		if (!Destination.Valid) { Destination = Source; return; }
		Destination.Minimum = glm::min(Destination.Minimum, Source.Minimum);
		Destination.Maximum = glm::max(Destination.Maximum, Source.Maximum);
	}

	WorldBox TransformBox(const glm::mat4 &Model) {
		WorldBox Result;
		const auto Matrix = glm::dmat4(Model);
		for (unsigned Mask = 0; Mask < 8; ++Mask) {
			const glm::dvec3 Local{
				Mask & 1 ? 0.5 : -0.5, Mask & 2 ? 0.5 : -0.5, Mask & 4 ? 0.5 : -0.5};
			const auto Point = Matrix * glm::dvec4(Local, 1.0);
			if (!std::isfinite(Point.x) || !std::isfinite(Point.y) || !std::isfinite(Point.z) ||
				Point.w != 1.0) return {.Unbounded = true};
			Result.Minimum = glm::min(Result.Minimum, glm::dvec3(Point));
			Result.Maximum = glm::max(Result.Maximum, glm::dvec3(Point));
		}
		Result.Valid = true;
		return Result;
	}

	// A broad world-space AABB test. The generous margin covers float camera/model
	// arithmetic in OutsideView; an uncertain node is always descended.
	bool OutsideNode(const ClipPlanes &Planes, const WorldBox &Bounds) {
		if (!Bounds.Valid || Bounds.Unbounded) return false;
		const auto Center = (Bounds.Minimum + Bounds.Maximum) * 0.5;
		const auto Extent = (Bounds.Maximum - Bounds.Minimum) * 0.5;
		for (const auto &FloatPlane : Planes) {
			const auto Plane = glm::dvec4(FloatPlane);
			const auto Normal = glm::dvec3(Plane);
			const auto Radius = glm::dot(glm::abs(Normal), Extent);
			const auto Distance = glm::dot(Normal, Center) + Plane.w;
			const auto Magnitude = 1.0 + glm::dot(glm::abs(Normal), glm::abs(Center) + Extent) + std::abs(Plane.w);
			const auto Margin = 1e-4 * Magnitude;
			if (std::isfinite(Distance) && std::isfinite(Radius) && Distance + Radius < -Margin)
				return true;
		}
		return false;
	}

	struct Object {
		glm::mat4 Model{1.0f};
		WorldBox Bounds;
	};

	struct QueryCounters { std::uint64_t Nodes = 0, Skipped = 0, Exact = 0; };

	class VisibilityIndex {
	  public:
		explicit VisibilityIndex(std::vector<Object> &ObjectsValue) : Objects(ObjectsValue) {
			for (auto &Object : Objects) Object.Bounds = TransformBox(Object.Model);
			Order.resize(Objects.size());
			std::iota(Order.begin(), Order.end(), 0u);
			LeafOfObject.resize(Objects.size());
			Nodes.reserve((Objects.size() / LeafSize + 1) * 2);
			if (!Objects.empty()) BuildNode(0, static_cast<std::uint32_t>(Objects.size()), -1);
		}

		struct Node {
			WorldBox Bounds;
			std::array<std::int32_t, 4> Children{-1, -1, -1, -1};
			std::int32_t Parent = -1;
			std::uint32_t First = 0, Count = 0;
		};

		[[nodiscard]] std::size_t AuxiliaryBytes() const {
			return Nodes.capacity() * sizeof(Node) + Order.capacity() * sizeof(std::uint32_t) +
				LeafOfObject.capacity() * sizeof(std::uint32_t) + Dirty.capacity() * sizeof(std::uint8_t);
		}
		[[nodiscard]] std::size_t BoundsBytes() const { return Objects.capacity() * sizeof(WorldBox); }

		void Refit(const std::vector<std::uint32_t> &Changed) {
			Dirty.assign(Nodes.size(), 0);
			for (const auto ObjectIndex : Changed) {
				Objects[ObjectIndex].Bounds = TransformBox(Objects[ObjectIndex].Model);
				for (auto NodeIndex = static_cast<std::int32_t>(LeafOfObject[ObjectIndex]);
					NodeIndex >= 0; NodeIndex = Nodes[NodeIndex].Parent)
					Dirty[NodeIndex] = 1;
			}
			for (std::size_t Index = Nodes.size(); Index > 0; --Index) {
				if (!Dirty[Index - 1]) continue;
				Refresh(static_cast<std::uint32_t>(Index - 1));
			}
		}

		void Query(const ClipPlanes &Planes, std::vector<std::uint8_t> &Visible,
			std::vector<std::uint32_t> &Ordered, QueryCounters &Counters) const {
			Visible.assign(Objects.size(), 0);
			Ordered.clear();
			Counters = {};
			if (Nodes.empty()) return;
			std::array<std::uint32_t, 64> Stack{};
			std::size_t StackSize = 1;
			Stack[0] = 0;
			while (StackSize) {
				const auto NodeIndex = Stack[--StackSize];
				const auto &Current = Nodes[NodeIndex];
				++Counters.Nodes;
				if (OutsideNode(Planes, Current.Bounds)) { Counters.Skipped += Current.Count; continue; }
				if (Current.Children[0] < 0) {
					for (std::uint32_t Position = Current.First; Position < Current.First + Current.Count; ++Position) {
						const auto ObjectIndex = Order[Position];
						++Counters.Exact;
						if (!OutsideView(Planes, Objects[ObjectIndex].Model, PrimitiveBounds))
							Visible[ObjectIndex] = 1;
					}
				} else for (const auto Child : Current.Children) if (Child >= 0) {
					if (StackSize == Stack.size()) throw std::logic_error("Visibility query stack exceeded bound");
					Stack[StackSize++] = static_cast<std::uint32_t>(Child);
				}
			}
			// Reconstruct the exact original traversal and depth-tie priority.
			for (std::uint32_t Index = 0; Index < Objects.size(); ++Index)
				if (Visible[Index]) Ordered.push_back(Index);
		}

	  private:
		std::vector<Object> &Objects;
		std::vector<Node> Nodes;
		std::vector<std::uint32_t> Order, LeafOfObject;
		std::vector<std::uint8_t> Dirty;

		std::uint32_t BuildNode(std::uint32_t First, std::uint32_t Count, std::int32_t Parent) {
			const auto NodeIndex = static_cast<std::uint32_t>(Nodes.size());
			Nodes.push_back({.Parent = Parent, .First = First, .Count = Count});
			if (Count <= LeafSize) {
				for (std::uint32_t Position = First; Position < First + Count; ++Position)
					LeafOfObject[Order[Position]] = NodeIndex;
				Refresh(NodeIndex);
				return NodeIndex;
			}
			WorldBox Centers;
			for (std::uint32_t Position = First; Position < First + Count; ++Position) {
				const auto &Box = Objects[Order[Position]].Bounds;
				if (!Box.Valid) continue;
				const auto Midpoint = (Box.Minimum + Box.Maximum) * 0.5;
				Include(Centers, {Midpoint, Midpoint, true});
			}
			const auto Span = Centers.Valid ? Centers.Maximum - Centers.Minimum : glm::dvec3(0);
			const unsigned Axis = Span.y > Span.x && Span.y >= Span.z ? 1u : Span.z > Span.x ? 2u : 0u;
			std::sort(Order.begin() + First, Order.begin() + First + Count, [&](std::uint32_t A, std::uint32_t B) {
				const auto &Left = Objects[A].Bounds;
				const auto &Right = Objects[B].Bounds;
				const auto CenterA = Left.Valid ? (Left.Minimum[Axis] + Left.Maximum[Axis]) * 0.5 : 0.0;
				const auto CenterB = Right.Valid ? (Right.Minimum[Axis] + Right.Maximum[Axis]) * 0.5 : 0.0;
				return CenterA != CenterB ? CenterA < CenterB : A < B;
			});
			for (unsigned ChildIndex = 0; ChildIndex < 4; ++ChildIndex) {
				const auto Offset = static_cast<std::uint32_t>((static_cast<std::uint64_t>(Count) * ChildIndex) / 4);
				const auto End = static_cast<std::uint32_t>((static_cast<std::uint64_t>(Count) * (ChildIndex + 1)) / 4);
				Nodes[NodeIndex].Children[ChildIndex] = static_cast<std::int32_t>(BuildNode(
					First + Offset, End - Offset, static_cast<std::int32_t>(NodeIndex)));
			}
			Refresh(NodeIndex);
			return NodeIndex;
		}

		void Refresh(std::uint32_t NodeIndex) {
			auto &Current = Nodes[NodeIndex];
			WorldBox Combined;
			if (Current.Children[0] < 0) {
				for (std::uint32_t Position = Current.First; Position < Current.First + Current.Count; ++Position)
					Include(Combined, Objects[Order[Position]].Bounds);
			} else for (const auto Child : Current.Children) if (Child >= 0)
				Include(Combined, Nodes[static_cast<std::size_t>(Child)].Bounds);
			Current.Bounds = Combined;
		}
	};

	RenderCameraSnapshot Camera(float Distance, glm::vec3 Offset = {}) {
		RenderCameraSnapshot Result;
		Result.Position = Offset + glm::vec3(0, 0, Distance);
		Result.NearPlane = 0.1f;
		Result.FarPlane = Distance * 4.0f;
		Result.VerticalFieldOfView = 45.0f;
		Result.ViewMatrix = glm::lookAtRH(Result.Position, Offset, glm::vec3(0, 1, 0));
		Result.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Result.VerticalFieldOfView),
			16.0f / 9.0f, Result.NearPlane, Result.FarPlane);
		Result.ViewProjectionMatrix = Result.ProjectionMatrix * Result.ViewMatrix;
		return Result;
	}

	void Linear(const std::vector<Object> &Objects, const ClipPlanes &Planes,
		std::vector<std::uint32_t> &Ordered) {
		Ordered.clear();
		for (std::uint32_t Index = 0; Index < Objects.size(); ++Index)
			if (!OutsideView(Planes, Objects[Index].Model, PrimitiveBounds)) Ordered.push_back(Index);
	}

	WorldBox BuildEnvelope(const std::vector<Object> &Objects) {
		WorldBox Result;
		for (const auto &Object : Objects) Include(Result, TransformBox(Object.Model));
		return Result;
	}

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	void VerifyAdversarial() {
		std::vector<Object> Objects(768);
		std::uint32_t Random = 0x91b32a19u;
		const auto Next = [&] { Random ^= Random << 13; Random ^= Random >> 17; Random ^= Random << 5;
			return static_cast<float>(Random % 100000) / 100000.0f; };
		for (std::size_t Index = 0; Index < Objects.size(); ++Index) {
			const auto Scale = Index % 19 == 0 ? glm::vec3(1e4f, 1e-5f, 1e-5f) : glm::vec3(0.1f + Next() * 2.0f);
			const auto Position = glm::vec3((Next() - 0.5f) * 200, (Next() - 0.5f) * 200,
				(Next() - 0.5f) * 200);
			Objects[Index].Model = glm::translate(glm::mat4(1), Position) *
				glm::rotate(glm::mat4(1), Next() * 6.0f, glm::normalize(glm::vec3(0.3f, 1, 0.7f))) *
				glm::scale(glm::mat4(1), Scale);
			Objects[Index].Bounds = TransformBox(Objects[Index].Model);
		}
		for (std::size_t Index = 0; Index < 16; ++Index) {
			const auto Position = glm::vec3(1e6f + static_cast<float>(Index) * 0.25f,
				1e6f, 1e6f + (Index % 2 ? 3.399f : 3.401f));
			Objects[Index].Model = glm::translate(glm::mat4(1), Position);
			Objects[Index].Bounds = TransformBox(Objects[Index].Model);
		}
		VisibilityIndex Index(Objects);
		auto Envelope = BuildEnvelope(Objects);
		std::vector<std::uint8_t> Bits;
		std::vector<std::uint32_t> Expected, Actual, EnvelopeActual;
		for (unsigned Step = 0; Step < 40; ++Step) {
			if (Step % 7 == 0) {
				const auto Changed = static_cast<std::uint32_t>((Step * 13) % Objects.size());
				Objects[Changed].Model[3].x += Step & 1 ? -0.125f : 0.125f;
				Index.Refit(std::vector<std::uint32_t>{Changed});
				Include(Envelope, TransformBox(Objects[Changed].Model));
			}
			const auto Offset = Step % 5 == 0 ? glm::vec3(1e6f) : glm::vec3(0);
			auto View = Camera(Step % 5 == 0 ? 4.0f : 80.0f, Offset);
			if (Step % 5 != 0) {
				const auto Target = glm::vec3((Next() - 0.5f) * 120, (Next() - 0.5f) * 120, 0);
				View.ViewMatrix = glm::lookAtRH(View.Position, Target, glm::vec3(0, 1, 0));
				View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			}
			const auto Planes = GetClipPlanes(View);
			Linear(Objects, Planes, Expected);
			QueryCounters Counters;
			Index.Query(Planes, Bits, Actual, Counters);
			if (Actual != Expected) throw std::runtime_error("Hierarchy changed original-order visibility in adversarial camera cases");
			EnvelopeActual.clear();
			if (!OutsideNode(Planes, Envelope)) Linear(Objects, Planes, EnvelopeActual);
			if (EnvelopeActual != Expected)
				throw std::runtime_error("Envelope changed original-order visibility in adversarial camera cases");
		}
		std::cout << "[Renderer:VisibilityProbe] adversarial-order=PASS cameras=40 objects=768 refits=6\n";
	}

	std::vector<Object> Fixture(std::uint32_t Count, std::uint32_t CulledPercent) {
		std::vector<Object> Objects(Count);
		const auto Side = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
		const auto Span = 2.0f * Side;
		const auto Distance = Span * 1.5f;
		for (std::uint32_t Index = 0; Index < Count; ++Index) {
			const bool Culled = Index % 100 < CulledPercent;
			const glm::vec3 Position{2.0f * (Index % Side) - (Span - 2.0f) * 0.5f,
				2.0f * (Index / Side) - (Span - 2.0f) * 0.5f, Culled ? Distance + 5.0f : 0.0f};
			Objects[Index].Model = glm::translate(glm::mat4(1), Position);
		}
		return Objects;
	}

	void Run(std::uint32_t Count, std::string_view Scenario, std::uint32_t CulledPercent,
		std::uint32_t DynamicPercent, bool RotateCamera = false) {
		auto Objects = Fixture(Count, CulledPercent);
		const auto Side = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
		auto View = Camera(3.0f * Side);
		if (RotateCamera) {
			View.ViewMatrix = glm::lookAtRH(View.Position, glm::vec3(1.5f * Side, 0, 0), glm::vec3(0, 1, 0));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
		}
		const auto Planes = GetClipPlanes(View);
		const auto BuildStart = Clock::now();
		VisibilityIndex Index(Objects);
		const auto BuildMs = Milliseconds(Clock::now() - BuildStart);
		const auto EnvelopeBuildStart = Clock::now();
		auto Envelope = BuildEnvelope(Objects);
		const auto EnvelopeBuildMs = Milliseconds(Clock::now() - EnvelopeBuildStart);
		std::vector<std::uint32_t> Changed, Expected, Actual, EnvelopeActual;
		std::vector<std::uint8_t> Bits;
		Expected.reserve(Count); Actual.reserve(Count); EnvelopeActual.reserve(Count); Changed.reserve(Count);
		std::array<double, 8> LinearSamples{}, IndexSamples{}, RefitSamples{}, EnvelopeSamples{}, EnvelopeRefitSamples{};
		std::uint32_t VisibleCount = 0;
		QueryCounters LastCounters;
		for (unsigned Iteration = 0; Iteration < LinearSamples.size(); ++Iteration) {
			Changed.clear();
			for (std::uint32_t ObjectIndex = 0; ObjectIndex < Count; ++ObjectIndex) {
				if (ObjectIndex % 100 >= DynamicPercent) continue;
				Objects[ObjectIndex].Model[3].x += (Iteration & 1 ? -0.125f : 0.125f);
				Changed.push_back(ObjectIndex);
			}
			const auto RefitStart = Clock::now();
			if (!Changed.empty()) Index.Refit(Changed);
			RefitSamples[Iteration] = Milliseconds(Clock::now() - RefitStart);
			const auto EnvelopeRefitStart = Clock::now();
			for (const auto ObjectIndex : Changed) Include(Envelope, TransformBox(Objects[ObjectIndex].Model));
			EnvelopeRefitSamples[Iteration] = Milliseconds(Clock::now() - EnvelopeRefitStart);
			const auto RunLinear = [&] {
				const auto Start = Clock::now();
				Linear(Objects, Planes, Expected);
				LinearSamples[Iteration] = Milliseconds(Clock::now() - Start);
			};
			const auto RunIndex = [&] {
				const auto Start = Clock::now();
				Index.Query(Planes, Bits, Actual, LastCounters);
				IndexSamples[Iteration] = Milliseconds(Clock::now() - Start);
			};
			const auto RunEnvelope = [&] {
				const auto Start = Clock::now();
				EnvelopeActual.clear();
				if (!OutsideNode(Planes, Envelope)) Linear(Objects, Planes, EnvelopeActual);
				EnvelopeSamples[Iteration] = Milliseconds(Clock::now() - Start);
			};
			switch (Iteration % 3) {
				case 0: RunLinear(); RunIndex(); RunEnvelope(); break;
				case 1: RunEnvelope(); RunLinear(); RunIndex(); break;
				default: RunIndex(); RunEnvelope(); RunLinear(); break;
			}
			if (Actual != Expected) throw std::runtime_error("Hierarchy changed original-order visibility in workload");
			if (EnvelopeActual != Expected) throw std::runtime_error("Envelope changed original-order visibility in workload");
			VisibleCount = static_cast<std::uint32_t>(Actual.size());
			Sink.fetch_xor(VisibleCount + LastCounters.Exact, std::memory_order_relaxed);
		}
		const auto Median = [](const auto &Values) {
			std::array<double, 6> Retained{};
			std::copy(Values.begin() + 2, Values.end(), Retained.begin());
			std::sort(Retained.begin(), Retained.end());
			return (Retained[2] + Retained[3]) * 0.5;
		};
		const auto LinearMs = Median(LinearSamples), QueryMs = Median(IndexSamples), RefitMs = Median(RefitSamples);
		const auto EnvelopeMs = Median(EnvelopeSamples), EnvelopeRefitMs = Median(EnvelopeRefitSamples);
		const auto Savings = LinearMs - QueryMs - RefitMs;
		const auto EnvelopeSavings = LinearMs - EnvelopeMs - EnvelopeRefitMs;
		std::cout << Scenario << ',' << Count << ',' << BuildMs << ',' << RefitMs << ',' << LinearMs << ','
			<< QueryMs << ',' << QueryMs + RefitMs << ',' << VisibleCount << ',' << LastCounters.Nodes << ','
			<< LastCounters.Skipped << ',' << LastCounters.Exact << ',' << Index.AuxiliaryBytes() << ','
			<< Index.BoundsBytes() << ',' << Index.AuxiliaryBytes() + Index.BoundsBytes() << ','
			<< EnvelopeBuildMs << ',' << EnvelopeRefitMs << ',' << EnvelopeMs << ',' << EnvelopeMs + EnvelopeRefitMs << ',';
		if (Savings > 0) std::cout << BuildMs / Savings;
		else std::cout << "NA";
		std::cout << ',' << (Savings > 0 ? "candidate" : "reject") << ',';
		if (EnvelopeSavings > 0) std::cout << EnvelopeBuildMs / EnvelopeSavings;
		else std::cout << "NA";
		std::cout << ',' << (EnvelopeSavings > 0 ? "candidate" : "reject") << '\n';
	}
}

int main() {
	try {
		VerifyAdversarial();
		std::cout << "Scenario,Objects,BuildMs,RefitMedianMs,LinearMedianMs,HierarchyQueryMedianMs,HierarchyTotalMedianMs,"
			"VisibleObjects,NodeTests,NodeSkippedObjects,ExactObjectTests,IndexAuxBytes,WorldBoundsBytes,"
			"TotalAdditionalBytes,EnvelopeBuildMs,EnvelopeRefitMedianMs,EnvelopeQueryMedianMs,EnvelopeTotalMedianMs,"
			"BuildBreakEvenFrames,Decision,EnvelopeBreakEvenFrames,EnvelopeDecision\n";
		for (const auto Count : {1000u, 10000u, 25000u, 50000u, 100000u}) {
			Run(Count, "static-visible", 0, 0);
			Run(Count, "camera-culled", 100, 0);
			Run(Count, "camera-rotated", 0, 0, true);
			Run(Count, "mixed", 50, 0);
			Run(Count, "mixed-dynamic-1", 50, 1);
			Run(Count, "mixed-dynamic-100", 50, 100);
		}
		std::cout << "[Renderer:VisibilityProbe] sink=" << Sink.load() << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Renderer:VisibilityProbe] " << Error.what() << '\n';
		return 1;
	}
}
