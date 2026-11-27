// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {
	using Slot = std::uint32_t;
	constexpr Slot Missing = std::numeric_limits<Slot>::max();
	constexpr std::size_t Rounds = 101;
	using Clock = std::chrono::steady_clock;

	void Require(bool Condition) {
		if (!Condition) {
			std::cerr << "[Terrain:Metadata] benchmark invariant failed\n";
			std::exit(2);
		}
	}

	struct Key {
		std::int64_t X;
		std::int64_t Y;
		std::int64_t Z;

		bool operator==(const Key &) const = default;
	};

	struct KeyHash {
		std::size_t operator()(const Key &Value) const noexcept {
			const std::uint64_t A = static_cast<std::uint64_t>(Value.X) * 0x9e3779b97f4a7c15ull;
			const std::uint64_t B = static_cast<std::uint64_t>(Value.Y) * 0xbf58476d1ce4e5b9ull;
			const std::uint64_t C = static_cast<std::uint64_t>(Value.Z) * 0x94d049bb133111ebull;
			return static_cast<std::size_t>(A ^ std::rotl(B, 21) ^ std::rotl(C, 42));
		}
	};

	Key MakeKey(Slot Id) {
		return {static_cast<std::int64_t>(Id % 64) - 32, static_cast<std::int64_t>(Id / 4096) - 8,
				static_cast<std::int64_t>(Id / 64 % 64) + (std::int64_t{1} << 37)};
	}

	class CountResource final : public std::pmr::memory_resource {
	  public:
		std::size_t Live = 0;
		std::size_t Peak = 0;
		std::size_t Allocations = 0;

	  private:
		void *do_allocate(std::size_t Bytes, std::size_t Alignment) override {
			void *Memory = std::pmr::new_delete_resource()->allocate(Bytes, Alignment);
			Live += Bytes;
			Peak = std::max(Peak, Live);
			++Allocations;
			return Memory;
		}

		void do_deallocate(void *Memory, std::size_t Bytes, std::size_t Alignment) override {
			Live -= Bytes;
			std::pmr::new_delete_resource()->deallocate(Memory, Bytes, Alignment);
		}

		bool do_is_equal(const std::pmr::memory_resource &Other) const noexcept override {
			return this == &Other;
		}
	};

	struct Value {
		std::uint64_t Geometry = 1;
		std::uint64_t Material = 1;
		std::uint32_t Render = 0;
		std::uint32_t Collision = 0;
	};

	struct Sum {
		std::uint64_t Count = 0;
		std::uint64_t Value = 0;

		void Add(Slot Id, std::uint64_t Data) {
			++Count;
			Value += std::rotl(Data * 0xbf58476d1ce4e5b9ull + Id, Id % 64);
		}

		bool operator==(const Sum &) const = default;
	};

	std::uint64_t Hash(Value Data) {
		return Data.Geometry + 17u * Data.Material + 29u * Data.Render + 47u * Data.Collision;
	}

	// An owning table. Archetype tables omit columns unavailable in that archetype.
	struct Columns {
		std::pmr::vector<Slot> Slots;
		std::pmr::vector<std::uint64_t> Geometry;
		std::pmr::vector<std::uint64_t> Material;
		std::pmr::vector<std::uint32_t> Render;
		std::pmr::vector<std::uint32_t> Collision;
		bool HasRender;
		bool HasCollision;

		Columns(CountResource &Resource, bool StoreRender, bool StoreCollision)
			: Slots(&Resource), Geometry(&Resource), Material(&Resource), Render(&Resource),
			  Collision(&Resource), HasRender(StoreRender), HasCollision(StoreCollision) {}

		void Reserve(Slot Count) {
			Slots.reserve(Count);
			Geometry.reserve(Count);
			Material.reserve(Count);
			if (HasRender)
				Render.reserve(Count);
			if (HasCollision)
				Collision.reserve(Count);
		}

		Value Get(Slot Row) const {
			return {Geometry[Row], Material[Row], HasRender ? Render[Row] : 0,
					HasCollision ? Collision[Row] : 0};
		}

		void Set(Slot Row, Value Data) {
			Geometry[Row] = Data.Geometry;
			Material[Row] = Data.Material;
			if (HasRender)
				Render[Row] = Data.Render;
			if (HasCollision)
				Collision[Row] = Data.Collision;
		}

		Slot Add(Slot Id, Value Data) {
			const Slot Row = static_cast<Slot>(Slots.size());
			Slots.push_back(Id);
			Geometry.push_back(Data.Geometry);
			Material.push_back(Data.Material);
			if (HasRender)
				Render.push_back(Data.Render);
			if (HasCollision)
				Collision.push_back(Data.Collision);
			return Row;
		}

		Slot Remove(Slot Row) {
			const Slot Last = static_cast<Slot>(Slots.size() - 1);
			const Slot Moved = Slots[Last];
			Set(Row, Get(Last));
			Slots[Row] = Moved;
			Slots.pop_back();
			Geometry.pop_back();
			Material.pop_back();
			if (HasRender)
				Render.pop_back();
			if (HasCollision)
				Collision.pop_back();
			return Moved;
		}

		void Project(bool ForRender, bool FilterRows, Sum &Result) const {
			const auto &Resources = ForRender ? Render : Collision;
			for (Slot Row = 0; Row < Resources.size(); ++Row) {
				if (!FilterRows || Resources[Row] != 0)
					Result.Add(Slots[Row], Geometry[Row] + 17u * Material[Row] + 29u * Resources[Row]);
			}
		}
	};

	class ActiveSoA {
	  public:
		static constexpr std::string_view Name = "SoA";
		ActiveSoA(CountResource &Resource, Slot Capacity) : Rows(Resource, true, true), Locations(&Resource) {
			Rows.Reserve(Capacity);
			Locations.resize(Capacity, Missing);
		}

		void Add(Slot Id, Value Data) { Locations[Id] = Rows.Add(Id, Data); }
		void Remove(Slot Id) {
			const Slot Row = Locations[Id];
			Locations[Rows.Remove(Row)] = Row;
			Locations[Id] = Missing;
		}
		bool Contains(Slot Id) const { return Locations[Id] != Missing; }
		Value Get(Slot Id) const { return Rows.Get(Locations[Id]); }
		void Update(Slot Id, Value Data) { Rows.Set(Locations[Id], Data); }
		template <typename Visit> void ForEach(Visit Function) const {
			for (Slot Id : Rows.Slots)
				Function(Id);
		}
		Sum Project(bool ForRender) const {
			Sum Result;
			Rows.Project(ForRender, true, Result);
			return Result;
		}

	  private:
		Columns Rows;
		std::pmr::vector<Slot> Locations;
	};

	template <typename T> class SparsePool {
	  public:
		SparsePool(CountResource &Resource, Slot Capacity, Slot ReserveCount)
			: Owners(&Resource), Values(&Resource), Locations(&Resource) {
			Owners.reserve(ReserveCount);
			Values.reserve(ReserveCount);
			Locations.resize(Capacity, Missing);
		}

		bool Contains(Slot Id) const { return Locations[Id] != Missing; }
		T Get(Slot Id) const { return Contains(Id) ? Values[Locations[Id]] : T{}; }

		void Insert(Slot Id, T Data) {
			Locations[Id] = static_cast<Slot>(Owners.size());
			Owners.push_back(Id);
			Values.push_back(Data);
		}

		void Set(Slot Id, T Data) {
			if (Contains(Id))
				Values[Locations[Id]] = Data;
			else
				Insert(Id, Data);
		}

		void Remove(Slot Id) {
			const Slot Row = Locations[Id];
			if (Row == Missing)
				return;
			Locations[Owners.back()] = Row;
			Owners[Row] = Owners.back();
			Values[Row] = Values.back();
			Locations[Id] = Missing;
			Owners.pop_back();
			Values.pop_back();
		}

		void SetOptional(Slot Id, T Data) {
			if (Data == T{})
				Remove(Id);
			else
				Set(Id, Data);
		}

		std::span<const Slot> GetOwners() const { return Owners; }
		std::span<const T> GetValues() const { return Values; }

	  private:
		std::pmr::vector<Slot> Owners;
		std::pmr::vector<T> Values;
		std::pmr::vector<Slot> Locations;
	};

	class SeparateComponents {
	  public:
		SeparateComponents(CountResource &Resource, Slot Capacity)
			: Geometry(Resource, Capacity, Capacity), Material(Resource, Capacity, Capacity) {}

		void Add(Slot Id, Value Data) {
			Geometry.Insert(Id, Data.Geometry);
			Material.Insert(Id, Data.Material);
		}
		void Remove(Slot Id) {
			Geometry.Remove(Id);
			Material.Remove(Id);
		}
		bool Contains(Slot Id) const { return Geometry.Contains(Id); }
		Value Get(Slot Id) const { return {Geometry.Get(Id), Material.Get(Id)}; }
		void Update(Slot Id, Value Data) {
			Geometry.Set(Id, Data.Geometry);
			Material.Set(Id, Data.Material);
		}
		template <typename Visit> void ForEach(Visit Function) const {
			for (Slot Id : Geometry.GetOwners())
				Function(Id);
		}

	  private:
		SparsePool<std::uint64_t> Geometry;
		SparsePool<std::uint64_t> Material;
	};

	class PackedComponents {
	  public:
		PackedComponents(CountResource &Resource, Slot Capacity) : Rows(Resource, false, false), Locations(&Resource) {
			Rows.Reserve(Capacity);
			Locations.resize(Capacity, Missing);
		}
		void Add(Slot Id, Value Data) { Locations[Id] = Rows.Add(Id, Data); }
		void Remove(Slot Id) {
			const Slot Row = Locations[Id];
			Locations[Rows.Remove(Row)] = Row;
			Locations[Id] = Missing;
		}
		bool Contains(Slot Id) const { return Locations[Id] != Missing; }
		Value Get(Slot Id) const { return Rows.Get(Locations[Id]); }
		void Update(Slot Id, Value Data) { Rows.Set(Locations[Id], Data); }
		template <typename Visit> void ForEach(Visit Function) const {
			for (Slot Id : Rows.Slots)
				Function(Id);
		}

	  private:
		Columns Rows;
		std::pmr::vector<Slot> Locations;
	};

	template <typename Core> class ComponentStorage {
	  public:
		static constexpr std::string_view Name = std::is_same_v<Core, SeparateComponents> ? "SparseSets" : "Hybrid";
		ComponentStorage(CountResource &Resource, Slot Capacity)
			: Hot(Resource, Capacity), Render(Resource, Capacity, Capacity / 2),
			  Collision(Resource, Capacity, Capacity / 2) {}

		void Add(Slot Id, Value Data) {
			Hot.Add(Id, Data);
			Render.SetOptional(Id, Data.Render);
			Collision.SetOptional(Id, Data.Collision);
		}
		void Remove(Slot Id) {
			Hot.Remove(Id);
			Render.Remove(Id);
			Collision.Remove(Id);
		}
		bool Contains(Slot Id) const { return Hot.Contains(Id); }
		Value Get(Slot Id) const {
			Value Data = Hot.Get(Id);
			Data.Render = Render.Get(Id);
			Data.Collision = Collision.Get(Id);
			return Data;
		}
		void Update(Slot Id, Value Data) {
			Hot.Update(Id, Data);
			Render.SetOptional(Id, Data.Render);
			Collision.SetOptional(Id, Data.Collision);
		}
		template <typename Visit> void ForEach(Visit Function) const { Hot.ForEach(Function); }
		Sum Project(bool ForRender) const {
			Sum Result;
			const auto &Pool = ForRender ? Render : Collision;
			const auto Owners = Pool.GetOwners();
			const auto Values = Pool.GetValues();
			for (Slot Row = 0; Row < Owners.size(); ++Row) {
				const Slot Id = Owners[Row];
				const Value Data = Hot.Get(Id);
				Result.Add(Id, Data.Geometry + 17u * Data.Material + 29u * Values[Row]);
			}
			return Result;
		}

	  private:
		Core Hot;
		SparsePool<std::uint32_t> Render;
		SparsePool<std::uint32_t> Collision;
	};

	class Archetypes {
	  public:
		static constexpr std::string_view Name = "Archetypes";
		Archetypes(CountResource &Resource, Slot Capacity)
			: Tables{Columns(Resource, false, false), Columns(Resource, true, false),
					 Columns(Resource, false, true), Columns(Resource, true, true)}, Locations(&Resource) {
			for (Columns &Table : Tables)
				Table.Reserve(Capacity / 4);
			Locations.resize(Capacity);
		}

		void Add(Slot Id, Value Data) {
			const unsigned Type = GetType(Data);
			Locations[Id] = {Tables[Type].Add(Id, Data), Type};
		}
		void Remove(Slot Id) {
			const Location At = Locations[Id];
			Locations[Tables[At.Type].Remove(At.Row)].Row = At.Row;
			Locations[Id].Row = Missing;
		}
		bool Contains(Slot Id) const { return Locations[Id].Row != Missing; }
		Value Get(Slot Id) const {
			const Location At = Locations[Id];
			return Tables[At.Type].Get(At.Row);
		}
		void Update(Slot Id, Value Data) {
			const Location At = Locations[Id];
			if (At.Type == GetType(Data)) {
				Tables[At.Type].Set(At.Row, Data);
			} else {
				Remove(Id);
				Add(Id, Data);
			}
		}
		template <typename Visit> void ForEach(Visit Function) const {
			for (const Columns &Table : Tables)
				for (Slot Id : Table.Slots)
					Function(Id);
		}
		Sum Project(bool ForRender) const {
			Sum Result;
			Tables[ForRender ? 1 : 2].Project(ForRender, false, Result);
			Tables[3].Project(ForRender, false, Result);
			return Result;
		}

	  private:
		struct Location {
			Slot Row = Missing;
			unsigned Type = 0;
		};

		static unsigned GetType(Value Data) {
			return static_cast<unsigned>(Data.Render != 0) | (static_cast<unsigned>(Data.Collision != 0) << 1u);
		}

		std::array<Columns, 4> Tables;
		std::pmr::vector<Location> Locations;
	};

	struct Ticket {
		Key Chunk;
		Slot LocalSlot;
		std::uint64_t WorldEpoch;
		std::uint64_t Generation;
		std::uint64_t Geometry;
		std::uint64_t Material;
	};

	struct JobOutput {
		Ticket Work;
		std::uint64_t Value;
	};

	// A worker sees only its captured value. It cannot access storage or provider state.
	JobOutput Prepare(Ticket Work) {
		return {Work, std::rotl(static_cast<std::uint64_t>(KeyHash{}(Work.Chunk)) + Work.Geometry,
								static_cast<int>(Work.Material % 64))};
	}

	struct Directory {
		std::pmr::unordered_map<Key, std::uint64_t, KeyHash> Inactive;
		std::pmr::unordered_map<Key, Slot, KeyHash> Active;
		CountResource &InactiveMemory;
		CountResource &ActiveMemory;

		Directory(CountResource &Cold, CountResource &Hot, Slot ActiveCount, Slot InactiveCount)
			: Inactive(&Cold), Active(&Hot), InactiveMemory(Cold), ActiveMemory(Hot) {
			Inactive.reserve(InactiveCount);
			Active.reserve(ActiveCount);
			for (Slot Id = 0; Id < ActiveCount; ++Id)
				Active.emplace(MakeKey(Id), Id);
			for (Slot Id = ActiveCount; Id < ActiveCount + InactiveCount; ++Id)
				Inactive.emplace(MakeKey(Id), Id + 1u);
		}
	};

	struct Trace {
		std::vector<Slot> QuerySlots;
		std::vector<Key> QueryKeys;
		std::vector<Slot> Churn;
		std::vector<Slot> Edited;
		std::vector<Slot> Recycle;

		Trace(Slot Active, Slot Inactive, Slot Round) {
			std::mt19937 Random(117655u + Round);
			for (Slot I = 0; I < 2048; ++I) {
				const Slot Id = Random() % Active;
				QuerySlots.push_back(Id);
				switch (I % 4) {
				case 0:
				case 1:
					QueryKeys.push_back(MakeKey(Id));
					break;
				case 2:
					QueryKeys.push_back(MakeKey(Active + Random() % Inactive));
					break;
				default:
					QueryKeys.push_back(MakeKey(Active + Inactive + Id));
					break;
				}
			}
			for (Slot I = 0; I < 256; ++I)
				Churn.push_back(Random() % Active);
			for (Slot I = 0; I < 1024; ++I) {
				const Slot X = Random() % Active;
				Edited.push_back(X);
				Edited.push_back(X ^ 1u);
				Edited.push_back(X ^ 64u);
				Edited.push_back(X ^ 65u);
			}
			for (Slot I = 0; I < 128; ++I)
				Recycle.push_back(Random() % Active);
		}
	};

	enum Phase : std::size_t {
		RenderProjection, CollisionProjection, RandomAccess, SpatialLookup, StructuralChurn,
		DirtyAndQueue, FullDirtyScan, WorkerPreparation, Lifecycle, DirectoryChurn, MainAdmission, PhaseCount
	};

	constexpr std::array<std::string_view, PhaseCount> PhaseNames = {
		"RenderProjection", "CollisionProjection", "RandomAccess", "SpatialLookup", "StructuralChurn",
		"DirtyAndQueue", "FullDirtyScan", "WorkerPreparation", "Lifecycle", "DirectoryChurn", "MainAdmission"
	};

	struct Measurement {
		std::array<Sum, PhaseCount> Sums;
		std::array<double, PhaseCount> Micros{};
	};

	template <typename Function>
	void Measure(Measurement &Result, Phase Index, Function &&Body) {
		const auto Start = Clock::now();
		Result.Sums[Index] = Body();
		const auto Finish = Clock::now();
		Result.Micros[Index] = std::chrono::duration<double, std::micro>(Finish - Start).count();
	}

	Value InitialValue(Slot Id, bool Skewed) {
		const unsigned Role = Skewed ? Id % 16u : Id % 4u;
		return {1, 1, (Role & 1u) == 0 ? 0 : Id + 1u,
				(Skewed ? Role == 15u : (Role & 2u) != 0) ? Id + 2u : 0};
	}

	template <typename Storage> class Fixture {
	  public:
		CountResource Memory;
		CountResource SchedulingMemory;
		Storage Records;
		std::array<std::vector<double>, PhaseCount> Times;

		Fixture(Slot Active, bool Skewed) : Records(Memory, Active), Generation(&SchedulingMemory),
				Marked(&SchedulingMemory), Dirty(&SchedulingMemory), Pending(&SchedulingMemory), Output(&SchedulingMemory) {
			Generation.resize(Active, 1);
			Marked.resize(Active, 0);
			Dirty.resize(Active);
			Pending.reserve(1024);
			Output.reserve(1024);
			for (Slot Id = 0; Id < Active; ++Id)
				Records.Add(Id, InitialValue(Id, Skewed));
		}

		Measurement Run(Directory &Index, const Trace &Inputs, Slot Round) {
			Measurement Result;
			Measure(Result, RenderProjection, [&] { return Records.Project(true); });
			Measure(Result, CollisionProjection, [&] { return Records.Project(false); });
			Measure(Result, RandomAccess, [&] {
				Sum Total;
				for (Slot Id : Inputs.QuerySlots)
					Total.Add(Id, Hash(Records.Get(Id)));
				return Total;
			});
			Measure(Result, SpatialLookup, [&] {
				Sum Total;
				for (const Key &Chunk : Inputs.QueryKeys) {
					const auto Live = Index.Active.find(Chunk);
					if (Live != Index.Active.end()) {
						Total.Add(Live->second, Hash(Records.Get(Live->second)));
					} else {
						const auto Cold = Index.Inactive.find(Chunk);
						Total.Add(0, Cold != Index.Inactive.end() ? Cold->second : 0);
					}
				}
				return Total;
			});
			Measure(Result, StructuralChurn, [&] {
				Sum Total;
				for (Slot Id : Inputs.Churn) {
					Value Data = Records.Get(Id);
					// Two value/role mutations with identical semantic state in all candidates.
					Data.Render = Data.Render == 0 ? Id + 1u : 0;
					Records.Update(Id, Data);
					Total.Add(Id, Hash(Data));
					Data.Render = Data.Render == 0 ? Id + 1u : 0;
					Records.Update(Id, Data);
				}
				return Total;
			});
			Measure(Result, DirtyAndQueue, [&] {
				Sum Total;
				Pending.clear();
				for (Slot Id : Inputs.Edited) {
					Value Data = Records.Get(Id);
					++Data.Geometry;
					Data.Material += (Id & 1u);
					Records.Update(Id, Data);
					if (Marked[Id] == 0) {
						Marked[Id] = 1;
						const std::size_t Tail = (DirtyHead + DirtySize) % Dirty.size();
						Dirty[Tail] = Id;
						++DirtySize;
					}
				}
				const auto AdmitCount = std::min<std::size_t>(DirtySize, 1024);
				for (std::size_t I = 0; I < AdmitCount; ++I) {
					const Slot Id = Dirty[DirtyHead];
					DirtyHead = (DirtyHead + 1) % Dirty.size();
					--DirtySize;
					Marked[Id] = 0;
					Ticket Work = Capture(Id);
					Pending.push_back(Work);
					Total.Add(Id, Work.Geometry + 17u * Work.Material + Work.Generation);
				}
				return Total;
			});
			Measure(Result, FullDirtyScan, [&] {
				Sum Total;
				Records.ForEach([&](Slot Id) {
					if (Marked[Id] != 0)
						Total.Add(Id, 1);
				});
				Require(Total.Count == DirtySize);
				return Total;
			});
			Measure(Result, WorkerPreparation, [&] {
				Sum Total;
				Output.clear();
				for (Ticket Work : Pending) {
					const auto Product = Prepare(Work);
					Output.push_back(Product);
					Total.Add(Work.LocalSlot, Product.Value);
				}
				return Total;
			});
			Measure(Result, Lifecycle, [&] {
				Sum Total;
				for (Slot Id : Inputs.Recycle) {
					const Ticket Old = Capture(Id);
					const Value Data = Records.Get(Id);
					Records.Remove(Id);
					Require(!IsCurrent(Old));
					++Generation[Id];
					Records.Add(Id, Data);
					Require(!IsCurrent(Old));
					Total.Add(Id, Generation[Id]);
				}
				return Total;
			});
			Measure(Result, DirectoryChurn, [&] {
				Sum Total;
				for (Slot Id : Inputs.Recycle) {
					const Key Chunk = MakeKey(Id);
					Require(Index.Active.erase(Chunk) == 1u);
					Require(Index.Active.emplace(Chunk, Id).second);
					Total.Add(Id, 1);
				}
				return Total;
			});
			Measure(Result, MainAdmission, [&] {
				Sum Total;
				for (const JobOutput &Product : Output) {
					if (IsCurrent(Product.Work))
						Total.Add(Product.Work.LocalSlot, Product.Value);
				}
				return Total;
			});
			if (Round > 0) {
				for (std::size_t I = 0; I < PhaseCount; ++I)
					Times[I].push_back(Result.Micros[I]);
			}
			return Result;
		}

		void VerifyWorkerLatch() const {
			const Ticket Current = Capture(0);
			Ticket WrongTerrain = Current;
			++WrongTerrain.WorldEpoch;
			Ticket OldGeometry = Current;
			--OldGeometry.Geometry;
			Ticket OldMaterial = Current;
			--OldMaterial.Material;
			Ticket WrongKey = Current;
			++WrongKey.Chunk.X;
			JobOutput Prepared{};
			std::jthread Worker([&] { Prepared = Prepare(Current); });
			Worker.join();
			Require(IsCurrent(Prepared.Work) && !IsCurrent(WrongTerrain) && !IsCurrent(OldGeometry) &&
					!IsCurrent(OldMaterial) && !IsCurrent(WrongKey));
		}

		void Print(std::string_view Scene, Slot Active, Slot Inactive, const Directory &Index) {
			for (std::size_t I = 0; I < PhaseCount; ++I) {
				auto &Samples = Times[I];
				std::ranges::sort(Samples);
				const auto Percentile = [&](std::size_t P) { return Samples[(P * Samples.size() + 99) / 100 - 1]; };
				std::cout << "Time," << Scene << ',' << Storage::Name << ',' << PhaseNames[I] << ',' << Samples.size()
						  << ',' << Percentile(50) << ',' << Percentile(95) << ',' << Percentile(99) << ','
						  << Samples.back() << '\n';
			}
			std::cout << "Storage," << Scene << ',' << Storage::Name << ',' << Active << ',' << Inactive << ','
					  << Memory.Live << ',' << Memory.Peak << ',' << Memory.Allocations << ','
					  << SchedulingMemory.Live << ',' << SchedulingMemory.Peak << ','
					  << Index.ActiveMemory.Live << ',' << Index.InactiveMemory.Live << '\n';
		}

	  private:
		Ticket Capture(Slot Id) const {
			const Value Data = Records.Get(Id);
			return {MakeKey(Id), Id, WorldEpoch, Generation[Id], Data.Geometry, Data.Material};
		}

		bool IsCurrent(Ticket Value) const {
			const Slot Id = Value.LocalSlot;
			if (Value.WorldEpoch != WorldEpoch || Id >= Generation.size() || Value.Chunk != MakeKey(Id) ||
				Generation[Id] != Value.Generation || !Records.Contains(Id))
				return false;
			const auto Data = Records.Get(Id);
			return Value.Geometry == Data.Geometry && Value.Material == Data.Material;
		}

		static constexpr std::uint64_t WorldEpoch = 47;
		std::pmr::vector<std::uint64_t> Generation;
		std::pmr::vector<std::uint8_t> Marked;
		std::pmr::vector<Slot> Dirty;
		std::size_t DirtyHead = 0;
		std::size_t DirtySize = 0;
		std::pmr::vector<Ticket> Pending;
		std::pmr::vector<JobOutput> Output;
	};

	void Compare(std::string_view Scene, Slot ActiveCount, Slot InactiveCount, bool Skewed) {
		CountResource Cold;
		CountResource Hot;
		Directory Index(Cold, Hot, ActiveCount, InactiveCount);
		Fixture<ActiveSoA> SoA(ActiveCount, Skewed);
		Fixture<ComponentStorage<SeparateComponents>> Sparse(ActiveCount, Skewed);
		Fixture<Archetypes> Tables(ActiveCount, Skewed);
		Fixture<ComponentStorage<PackedComponents>> Hybrid(ActiveCount, Skewed);
		for (Slot Round = 0; Round <= Rounds; ++Round) {
			Trace Inputs(ActiveCount, InactiveCount, Round);
			std::array<Measurement, 4> Observed;
			for (Slot Offset = 0; Offset < Observed.size(); ++Offset) {
				const Slot Variant = (Round + Offset) % 4;
				switch (Variant) {
				case 0: Observed[Variant] = SoA.Run(Index, Inputs, Round); break;
				case 1: Observed[Variant] = Sparse.Run(Index, Inputs, Round); break;
				case 2: Observed[Variant] = Tables.Run(Index, Inputs, Round); break;
				case 3: Observed[Variant] = Hybrid.Run(Index, Inputs, Round); break;
				}
			}
			for (std::size_t I = 1; I < Observed.size(); ++I)
				Require(Observed[I].Sums == Observed[0].Sums);
		}
		SoA.VerifyWorkerLatch();
		Sparse.VerifyWorkerLatch();
		Tables.VerifyWorkerLatch();
		Hybrid.VerifyWorkerLatch();
		SoA.Print(Scene, ActiveCount, InactiveCount, Index);
		Sparse.Print(Scene, ActiveCount, InactiveCount, Index);
		Tables.Print(Scene, ActiveCount, InactiveCount, Index);
		Hybrid.Print(Scene, ActiveCount, InactiveCount, Index);
	}
}

int main() {
	std::cout << "TimeHeader,Scene,Storage,Phase,Samples,P50_us,P95_us,P99_us,Worst_us\n";
	std::cout << "StorageHeader,Scene,Storage,Active,Inactive,RegistryBytes,RegistryPeakBytes,RegistryAllocations,"
				 "CommonSchedulerBytes,CommonSchedulerPeakBytes,ActiveDirectoryBytes,InactiveDirectoryBytes\n";
	Compare("Balanced", 1024, 16384, false);
	Compare("Skewed", 8192, 131072, true);
	std::cout << "Parity,AllCandidates,AllPhases,Pass\n";
}
