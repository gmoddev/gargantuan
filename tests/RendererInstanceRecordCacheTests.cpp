// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "render/sdl/SDLInstanceRecordCache.hpp"

#include <cmath>
#include <iostream>

namespace {
	int Failures = 0;
	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "[Renderer:InstanceRecordTest] " << Message << '\n';
		++Failures;
	}

	gargantuan::RenderFrameState Frame() { return {.ViewportWidth = 640, .ViewportHeight = 360}; }

	gargantuan::RenderObjectCreate Create(std::uint32_t Slot, std::uint32_t Generation = 1) {
		gargantuan::RenderObjectCreate Result;
		Result.Item.Object = {Slot, Generation};
		return Result;
	}

	gargantuan::RenderObjectUpdate Update(std::uint32_t Slot, float Position, std::uint32_t Generation = 1) {
		gargantuan::RenderObjectUpdate Result;
		Result.Object = {Slot, Generation};
		Result.Domains = gargantuan::RenderUpdateDomain::Transform;
		Result.Item.Object = Result.Object;
		Result.Item.ModelMatrix[3].x = Position;
		Result.Item.InverseModelMatrix[3].x = -Position;
		return Result;
	}
}

int main() {
	using namespace gargantuan;
	SDLInstanceRecordCache Cache(2.0f);
	RenderPublication Full{.Id = 1, .FullResync = true, .Frame = Frame()};
	for (std::uint32_t Slot = 1; Slot <= 100; ++Slot) Full.Creates.push_back(Create(Slot));
	Check(Cache.Apply(Full), "full resync admitted bounded records");
	Check(Cache.ActiveCount() == 100 && Cache.SlotCount() == 100, "full resync allocated one slot per identity");
	Check(Cache.PlanUpload().Full && Cache.PlanUpload().Bytes == 100 * sizeof(SDLInstanceRecord),
		"full resync requests complete first upload");
	Cache.MarkUploaded();
	Check(Cache.PlanUpload().Bytes == 0, "quiet publication does not upload records");

	RenderPublication Sparse{.Id = 2, .BaseId = 1, .Frame = Frame()};
	Sparse.Updates.push_back(Update(37, 4.0f));
	Sparse.Updates.push_back(Update(39, 8.0f));
	Check(Cache.Apply(Sparse), "sparse update admitted");
	const auto Slot37 = Cache.Find({37, 1});
	const auto Slot39 = Cache.Find({39, 1});
	const auto SparsePlan = Cache.PlanUpload();
	Check(Slot37 && Slot39 && SparsePlan.Ranges.size() == 2 && !SparsePlan.Full &&
		SparsePlan.Bytes == 2 * sizeof(SDLInstanceRecord), "two distant changes remain sparse uploads");
	Check(Slot37 && Cache.Get(*Slot37)->ModelMatrix[3].x == 4.0f,
		"transform update changes only its stable record");
	Cache.MarkUploaded();
	Check(Cache.Apply(Sparse) && Cache.PlanUpload().Bytes == 0,
		"repeated equal values do not dirty instance records");
	RenderPublication MaterialWithTransformDomain{.Id = 20, .BaseId = 2, .Frame = Frame()};
	auto MaterialChange = Update(37, 4.0f);
	MaterialChange.Material.BaseColorFactor.x = 0.25f;
	MaterialWithTransformDomain.Updates.push_back(MaterialChange);
	Check(Cache.Apply(MaterialWithTransformDomain) && Slot37 &&
		Cache.PlanUpload().Bytes == sizeof(SDLInstanceRecord) &&
		Cache.Get(*Slot37)->Color.x == 0.25f,
		"transform-domain publication still packs all changed material values");
	Cache.MarkUploaded();
	Check(Cache.Apply(MaterialWithTransformDomain) && Cache.PlanUpload().Bytes == 0,
		"bitwise equal full payload stays clean on repeat");
	RenderPublication PositiveZero{.Id = 21, .BaseId = 20, .Frame = Frame()};
	auto ZeroScale = Update(39, 8.0f);
	ZeroScale.Material.NormalScale = 0.0f;
	PositiveZero.Updates.push_back(ZeroScale);
	Check(Cache.Apply(PositiveZero), "positive zero update admitted");
	Cache.MarkUploaded();
	RenderPublication NegativeZero{.Id = 22, .BaseId = 21, .Frame = Frame()};
	ZeroScale.Material.NormalScale = -0.0f;
	NegativeZero.Updates.push_back(ZeroScale);
	Check(Cache.Apply(NegativeZero) && Slot39 &&
		Cache.PlanUpload().Bytes == sizeof(SDLInstanceRecord) &&
		std::signbit(Cache.Get(*Slot39)->MaterialControls.x),
		"signed-zero byte difference safely uploads the exact new payload");
	Cache.MarkUploaded();
	Check(Cache.Apply(NegativeZero) && Cache.PlanUpload().Bytes == 0,
		"repeating the new signed-zero payload remains clean");

	RenderPublication Replace{.Id = 3, .BaseId = 2, .Frame = Frame()};
	Replace.Removes.push_back({{37, 1}});
	Replace.Creates.push_back(Create(37, 2));
	Check(Cache.Apply(Replace) && !Cache.Find({37, 1}) && Cache.Find({37, 2}) == Slot37,
		"generation-safe removal and reinsertion reuse only the native slot");
	Cache.MarkUploaded();

	RenderPublication Dense{.Id = 4, .BaseId = 3, .Frame = Frame()};
	for (std::uint32_t Slot = 40; Slot < 70; ++Slot) Dense.Updates.push_back(Update(Slot, 1.0f));
	Check(Cache.Apply(Dense) && Cache.PlanUpload().Full &&
		Cache.PlanUpload().Bytes == 100 * sizeof(SDLInstanceRecord),
		"dense updates select one complete upload");
	Cache.MarkUploaded();
	Cache.MarkFullUpload();
	Check(Cache.PlanUpload().Full, "renderer recreation requests complete record upload");
	Cache.Clear();
	Check(Cache.ActiveCount() == 0 && Cache.PlanUpload().Bytes == 0, "clear retires all slots and upload work");

	RenderPublication Overflow{.Id = 5, .FullResync = true, .Frame = Frame()};
	for (std::uint32_t Slot = 1; Slot <= SDLInstanceRecordCache::MaximumRecords + 1; ++Slot)
		Overflow.Creates.push_back(Create(Slot));
	Check(!Cache.Apply(Overflow) && !Cache.IsAvailable() && Cache.ActiveCount() == 0,
		"capacity exhaustion disables only optional native instancing");
	Check(Cache.Apply(Full) && Cache.IsAvailable(), "next full resync restores optional record cache");
	RenderPublication OneRange{.Id = 6, .FullResync = true, .Frame = Frame()};
	auto Single = Create(101);
	Single.Material.BaseColorFactor = {1.0f, 0.0f, 0.0f, 1.0f};
	RenderPrimitiveMaterialState Range{};
	Range.Material.BaseColorFactor = {0.0f, 1.0f, 0.0f, 1.0f};
	Single.Primitives = std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
		std::initializer_list<RenderPrimitiveMaterialState>{Range});
	OneRange.Creates.push_back(Single);
	Check(Cache.Apply(OneRange) && Cache.Find({101, 1}) &&
		Cache.Get(*Cache.Find({101, 1}))->Color == Range.Material.BaseColorFactor,
		"one-range draw uses the authored primitive material");
	if (Failures) return 1;
	std::cout << "[Renderer:InstanceRecordTest] PASS\n";
}
