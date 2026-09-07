#include "gargantuan/runtime/SpatialRegionIndex.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		template <typename Value> void SaturatingIncrement(Value &Counter, Value Amount = 1) {
			Counter = Amount > std::numeric_limits<Value>::max() - Counter ? std::numeric_limits<Value>::max()
																		   : Counter + Amount;
		}

		bool Finite(glm::dvec3 Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		std::optional<std::int64_t> RegionCoordinate(double Value, double RegionSize) {
			if (!std::isfinite(Value) || !std::isfinite(RegionSize) || RegionSize <= 0.0) return std::nullopt;
			const long double Coordinate = std::floor(static_cast<long double>(Value) / RegionSize);
			if (!std::isfinite(Coordinate) || Coordinate < std::numeric_limits<std::int64_t>::min() ||
				Coordinate > std::numeric_limits<std::int64_t>::max())
				return std::nullopt;
			return static_cast<std::int64_t>(Coordinate);
		}

		std::optional<std::size_t> AxisCount(std::int64_t Minimum, std::int64_t Maximum, std::size_t Limit) {
			if (Maximum < Minimum) return std::nullopt;
			const long double Count = static_cast<long double>(Maximum) - static_cast<long double>(Minimum) + 1.0L;
			if (!std::isfinite(Count) || Count > static_cast<long double>(Limit)) return std::nullopt;
			return static_cast<std::size_t>(Count);
		}

		bool ContainsAddress(
			const SpatialCellAddress &Primary, std::span<const SpatialCellAddress> Additional, SpatialCellAddress Value
		) {
			return Primary == Value || std::ranges::binary_search(Additional, Value);
		}
	}

	std::uint64_t SpatialCellAddress::StableHash() const noexcept {
		std::uint64_t Hash = 14695981039346656037ull;
		auto Mix = [&Hash](std::uint64_t Value) {
			for (std::size_t Byte = 0; Byte < sizeof(Value); ++Byte) {
				Hash ^= (Value >> (Byte * 8)) & 0xffu;
				Hash *= 1099511628211ull;
			}
		};
		Mix(Space.Slot);
		Mix(Space.Generation);
		Mix(std::bit_cast<std::uint64_t>(Cell.X));
		Mix(std::bit_cast<std::uint64_t>(Cell.Y));
		Mix(std::bit_cast<std::uint64_t>(Cell.Z));
		return Hash;
	}

	std::string SpatialCellAddress::ToString() const {
		std::ostringstream Stream;
		Stream << "space=(" << Space.Slot << ',' << Space.Generation << ") cell=(" << Cell.X << ',' << Cell.Y << ','
			   << Cell.Z << ')';
		return Stream.str();
	}

	bool SpatialBounds::IsValid() const {
		return Finite(Minimum) && Finite(Maximum) && Minimum.x <= Maximum.x && Minimum.y <= Maximum.y &&
			   Minimum.z <= Maximum.z;
	}

	bool SpatialRegionQueryVolume::IsValid() const {
		return Space.IsValid() && Finite(Center) && std::isfinite(Radius) && Radius >= 0.0;
	}

	bool SpatialRegionIndexConfiguration::IsValid() const {
		return std::isfinite(RegionSize) && RegionSize >= 1.0 && RegionSize <= 1'048'576.0 && MaximumObjects != 0 &&
			   MaximumObjects <= MaximumSpatialRegionIndexObjects && MaximumRegions != 0 &&
			   MaximumRegions <= MaximumSpatialRegionIndexRegions && MaximumMemberships >= MaximumObjects &&
			   MaximumMemberships <= MaximumSpatialRegionIndexMemberships && MaximumRegionsPerObject != 0 &&
			   MaximumRegionsPerObject <= MaximumSpatialRegionsPerObject && MaximumLargeObjects != 0 &&
			   MaximumLargeObjects <= MaximumSpatialLargeObjects && MaximumQueryVolumes != 0 &&
			   MaximumQueryVolumes <= MaximumSpatialQueryVolumes && MaximumQueryRegions != 0 &&
			   MaximumQueryRegions <= MaximumSpatialQueryRegions && MaximumQueryCandidates != 0 &&
			   MaximumQueryCandidates <= MaximumSpatialQueryCandidates && MaximumQueryMembershipVisits != 0 &&
			   MaximumQueryMembershipVisits <= MaximumSpatialQueryMembershipVisits &&
			   MaximumQueryMembershipVisits >= MaximumQueryCandidates;
	}

	const char *SpatialRegionStatusName(SpatialRegionStatus Status) noexcept {
		switch (Status) {
		case SpatialRegionStatus::Success:
			return "success";
		case SpatialRegionStatus::InvalidConfiguration:
			return "invalid configuration";
		case SpatialRegionStatus::InvalidIdentity:
			return "invalid identity";
		case SpatialRegionStatus::InvalidSpace:
			return "invalid space";
		case SpatialRegionStatus::InvalidBounds:
			return "invalid bounds";
		case SpatialRegionStatus::InvalidCoordinate:
			return "invalid coordinate";
		case SpatialRegionStatus::DuplicateObject:
			return "duplicate object";
		case SpatialRegionStatus::MissingObject:
			return "missing object";
		case SpatialRegionStatus::ObjectLimit:
			return "object limit";
		case SpatialRegionStatus::RegionLimit:
			return "region limit";
		case SpatialRegionStatus::MembershipLimit:
			return "membership limit";
		case SpatialRegionStatus::LargeObjectLimit:
			return "large-object limit";
		case SpatialRegionStatus::QueryVolumeLimit:
			return "query-volume limit";
		case SpatialRegionStatus::QueryRegionLimit:
			return "query-region limit";
		case SpatialRegionStatus::QueryCandidateLimit:
			return "query-candidate limit";
		case SpatialRegionStatus::QueryMembershipVisitLimit:
			return "query-membership-visit limit";
		case SpatialRegionStatus::AllocationFailure:
			return "allocation failure";
		}
		return "unknown";
	}

	void SpatialRegionQueryScratch::Reserve(const SpatialRegionIndexConfiguration &Configuration) {
		Regions.reserve(Configuration.MaximumQueryRegions);
		Candidates.reserve(Configuration.MaximumQueryCandidates);
	}

	void SpatialRegionQueryScratch::Clear() {
		Regions.clear();
		Candidates.clear();
	}

	std::optional<SpatialCellAddress>
	SpatialCellAddressForPosition(glm::dvec3 Position, double RegionSize, SpatialSpaceId Space) {
		if (!Space.IsValid() || !Finite(Position)) return std::nullopt;
		const auto X = RegionCoordinate(Position.x, RegionSize);
		const auto Y = RegionCoordinate(Position.y, RegionSize);
		const auto Z = RegionCoordinate(Position.z, RegionSize);
		if (!X || !Y || !Z) return std::nullopt;
		return SpatialCellAddress{Space, {*X, *Y, *Z}};
	}

	struct SpatialRegionIndex::Implementation {
		struct Entry {
			SpatialCellAddress Primary;
			std::vector<SpatialCellAddress> Additional;
			std::uint64_t QueryGeneration = 0;
			bool Large = false;

			[[nodiscard]] std::size_t MembershipCount() const {
				return Large ? 0 : Additional.size() + 1;
			}
			[[nodiscard]] bool Contains(SpatialCellAddress Address) const {
				return !Large && ContainsAddress(Primary, Additional, Address);
			}
		};

		SpatialRegionIndexConfiguration Configuration;
		std::map<ObjectId, Entry> Objects;
		std::map<SpatialCellAddress, std::set<ObjectId>> Regions;
		std::map<SpatialSpaceId, std::set<ObjectId>> LargeObjects;
		std::vector<SpatialCellAddress> MembershipScratch;
		std::vector<SpatialCellAddress> AddedScratch;
		std::vector<SpatialCellAddress> RemovedScratch;
		std::uint64_t QueryGeneration = 0;
		SpatialRegionIndexMetrics Metrics;

		explicit Implementation(SpatialRegionIndexConfiguration ConfigurationValue)
			: Configuration(ConfigurationValue) {
			MembershipScratch.reserve(Configuration.MaximumRegionsPerObject);
			AddedScratch.reserve(Configuration.MaximumRegionsPerObject);
			RemovedScratch.reserve(Configuration.MaximumRegionsPerObject);
		}

		void RefreshGauges() {
			Metrics.RegionCount = Regions.size();
			Metrics.SpatialObjectCount = Objects.size();
			Metrics.LargeObjectCount = 0;
			for (const auto &[Space, ObjectsInSpace] : LargeObjects) {
				(void)Space;
				SaturatingIncrement(Metrics.LargeObjectCount, static_cast<std::uint64_t>(ObjectsInSpace.size()));
			}
		}

		std::span<const SpatialCellAddress> Additional(const Entry &Value) const {
			return Value.Additional;
		}

		void CopyMemberships(const Entry &Value, std::vector<SpatialCellAddress> &Destination) const {
			Destination.clear();
			if (Value.Large) return;
			Destination.push_back(Value.Primary);
			Destination.insert(Destination.end(), Value.Additional.begin(), Value.Additional.end());
		}

		SpatialRegionStatus DeriveMemberships(SpatialSpaceId Space, const SpatialBounds &Bounds, bool &Large) {
			MembershipScratch.clear();
			Large = false;
			if (!Space.IsValid()) return SpatialRegionStatus::InvalidSpace;
			if (!Bounds.IsValid()) return SpatialRegionStatus::InvalidBounds;
			const glm::dvec3 MaximumPoint{
				Bounds.Maximum.x > Bounds.Minimum.x
					? std::nextafter(Bounds.Maximum.x, -std::numeric_limits<double>::infinity())
					: Bounds.Maximum.x,
				Bounds.Maximum.y > Bounds.Minimum.y
					? std::nextafter(Bounds.Maximum.y, -std::numeric_limits<double>::infinity())
					: Bounds.Maximum.y,
				Bounds.Maximum.z > Bounds.Minimum.z
					? std::nextafter(Bounds.Maximum.z, -std::numeric_limits<double>::infinity())
					: Bounds.Maximum.z,
			};
			const auto Minimum = SpatialCellAddressForPosition(Bounds.Minimum, Configuration.RegionSize, Space);
			const auto Maximum = SpatialCellAddressForPosition(MaximumPoint, Configuration.RegionSize, Space);
			if (!Minimum || !Maximum) return SpatialRegionStatus::InvalidCoordinate;
			const auto XCount = AxisCount(Minimum->Cell.X, Maximum->Cell.X, Configuration.MaximumRegionsPerObject);
			const auto YCount = AxisCount(Minimum->Cell.Y, Maximum->Cell.Y, Configuration.MaximumRegionsPerObject);
			const auto ZCount = AxisCount(Minimum->Cell.Z, Maximum->Cell.Z, Configuration.MaximumRegionsPerObject);
			if (!XCount || !YCount || !ZCount || *XCount > Configuration.MaximumRegionsPerObject / *YCount ||
				*XCount * *YCount > Configuration.MaximumRegionsPerObject / *ZCount) {
				Large = true;
				return SpatialRegionStatus::Success;
			}
			for (std::int64_t X = Minimum->Cell.X;; ++X) {
				for (std::int64_t Y = Minimum->Cell.Y;; ++Y) {
					for (std::int64_t Z = Minimum->Cell.Z;; ++Z) {
						MembershipScratch.push_back({Space, {X, Y, Z}});
						if (Z == Maximum->Cell.Z) break;
					}
					if (Y == Maximum->Cell.Y) break;
				}
				if (X == Maximum->Cell.X) break;
			}
			return SpatialRegionStatus::Success;
		}

		Entry MakeEntry(SpatialSpaceId Space, const SpatialBounds &Bounds, bool Large) const {
			Entry Result{.Large = Large};
			const auto Center = Bounds.Minimum * 0.5 + Bounds.Maximum * 0.5;
			const auto CenterAddress = SpatialCellAddressForPosition(Center, Configuration.RegionSize, Space);
			if (CenterAddress) Result.Primary = *CenterAddress;
			if (!Large) {
				if (!CenterAddress || !std::ranges::binary_search(MembershipScratch, *CenterAddress))
					Result.Primary = MembershipScratch.front();
				Result.Additional.reserve(MembershipScratch.size() - 1);
				for (const auto Address : MembershipScratch)
					if (Address != Result.Primary) Result.Additional.push_back(Address);
			}
			return Result;
		}

		void UpdatePrimaryAddress(Entry &Value, SpatialSpaceId Space, const SpatialBounds &Bounds) const {
			const auto Center = Bounds.Minimum * 0.5 + Bounds.Maximum * 0.5;
			const auto Address = SpatialCellAddressForPosition(Center, Configuration.RegionSize, Space);
			if (!Address || Value.Primary == *Address) return;
			if (!Value.Large) {
				auto NewPrimary = std::ranges::lower_bound(Value.Additional, *Address);
				if (NewPrimary == Value.Additional.end() || *NewPrimary != *Address) return;
				Value.Additional.erase(NewPrimary);
				Value.Additional.insert(std::ranges::lower_bound(Value.Additional, Value.Primary), Value.Primary);
			}
			Value.Primary = *Address;
		}

		std::size_t NewRegionCount(std::span<const SpatialCellAddress> Addresses) const {
			return static_cast<std::size_t>(std::ranges::count_if(Addresses, [this](SpatialCellAddress Address) {
				return !Regions.contains(Address);
			}));
		}

		bool InsertMemberships(ObjectId Object, std::span<const SpatialCellAddress> Addresses) {
			try {
				for (const auto Address : Addresses) {
					auto [Region, Created] = Regions.try_emplace(Address);
					if (Created) SaturatingIncrement(Metrics.RegionBucketsCreated);
					Region->second.insert(Object);
					Metrics.PeakRegionOccupancy = std::max<std::uint64_t>(
						Metrics.PeakRegionOccupancy, static_cast<std::uint64_t>(Region->second.size())
					);
				}
				return true;
			} catch (const std::bad_alloc &) {
				for (const auto Address : Addresses) {
					auto Region = Regions.find(Address);
					if (Region == Regions.end()) continue;
					Region->second.erase(Object);
					if (Region->second.empty()) {
						Regions.erase(Region);
						SaturatingIncrement(Metrics.RegionBucketsRemoved);
					}
				}
				RefreshGauges();
				return false;
			}
		}

		void RemoveMemberships(ObjectId Object, std::span<const SpatialCellAddress> Addresses) {
			for (const auto Address : Addresses) {
				auto Region = Regions.find(Address);
				if (Region == Regions.end()) continue;
				Region->second.erase(Object);
				if (Region->second.empty()) {
					Regions.erase(Region);
					SaturatingIncrement(Metrics.RegionBucketsRemoved);
				}
			}
		}

		bool InsertLargeObject(SpatialSpaceId Space, ObjectId Object) {
			try {
				auto [Bucket, Created] = LargeObjects.try_emplace(Space);
				try {
					Bucket->second.insert(Object);
				} catch (...) {
					if (Created && Bucket->second.empty()) LargeObjects.erase(Bucket);
					throw;
				}
				return true;
			} catch (const std::bad_alloc &) {
				return false;
			}
		}

		void RemoveLargeObject(SpatialSpaceId Space, ObjectId Object) {
			auto Bucket = LargeObjects.find(Space);
			if (Bucket == LargeObjects.end()) return;
			Bucket->second.erase(Object);
			if (Bucket->second.empty()) LargeObjects.erase(Bucket);
		}

		bool ContainsLargeObject(const Entry &Value, ObjectId Object) const {
			auto Bucket = LargeObjects.find(Value.Primary.Space);
			return Bucket != LargeObjects.end() && Bucket->second.contains(Object);
		}

		SpatialRegionStatus Register(ObjectId Object, SpatialSpaceId Space, const SpatialBounds &Bounds) {
			if (!Object.IsValid()) return SpatialRegionStatus::InvalidIdentity;
			if (!Space.IsValid()) return SpatialRegionStatus::InvalidSpace;
			if (Objects.contains(Object)) return SpatialRegionStatus::DuplicateObject;
			if (Objects.size() >= Configuration.MaximumObjects) return SpatialRegionStatus::ObjectLimit;
			bool Large = false;
			if (const auto Status = DeriveMemberships(Space, Bounds, Large); Status != SpatialRegionStatus::Success)
				return Status;
			if (Large && Metrics.LargeObjectCount >= Configuration.MaximumLargeObjects)
				return SpatialRegionStatus::LargeObjectLimit;
			if (!Large && (MembershipScratch.size() > Configuration.MaximumMemberships ||
						   Metrics.MembershipCount > Configuration.MaximumMemberships - MembershipScratch.size()))
				return SpatialRegionStatus::MembershipLimit;
			if (!Large && NewRegionCount(MembershipScratch) > Configuration.MaximumRegions - Regions.size())
				return SpatialRegionStatus::RegionLimit;
			Entry Candidate;
			try {
				Candidate = MakeEntry(Space, Bounds, Large);
			} catch (const std::bad_alloc &) {
				return SpatialRegionStatus::AllocationFailure;
			}
			if (Large) {
				if (!InsertLargeObject(Space, Object)) return SpatialRegionStatus::AllocationFailure;
			} else if (!InsertMemberships(Object, MembershipScratch)) {
				return SpatialRegionStatus::AllocationFailure;
			}
			try {
				Objects.emplace(Object, std::move(Candidate));
			} catch (const std::bad_alloc &) {
				if (Large)
					RemoveLargeObject(Space, Object);
				else
					RemoveMemberships(Object, MembershipScratch);
				return SpatialRegionStatus::AllocationFailure;
			}
			if (!Large) SaturatingIncrement(Metrics.MembershipCount, MembershipScratch.size());
			SaturatingIncrement(Metrics.ObjectRegistrations);
			RefreshGauges();
			return SpatialRegionStatus::Success;
		}

		SpatialRegionStatus Update(ObjectId Object, SpatialSpaceId Space, const SpatialBounds &Bounds) {
			auto Found = Objects.find(Object);
			if (Found == Objects.end()) return SpatialRegionStatus::MissingObject;
			if (!Space.IsValid()) return SpatialRegionStatus::InvalidSpace;
			bool Large = false;
			if (const auto Status = DeriveMemberships(Space, Bounds, Large); Status != SpatialRegionStatus::Success)
				return Status;
			const auto OldMembershipCount = Found->second.MembershipCount();
			bool Same = Found->second.Large == Large && Found->second.Primary.Space == Space;
			if (Same && !Large) {
				Same = OldMembershipCount == MembershipScratch.size() &&
					   std::ranges::all_of(MembershipScratch, [&Found](SpatialCellAddress Address) {
						   return Found->second.Contains(Address);
					   });
			}
			if (Same) {
				UpdatePrimaryAddress(Found->second, Space, Bounds);
				SaturatingIncrement(Metrics.SameRegionUpdates);
				return SpatialRegionStatus::Success;
			}
			const auto OldSpace = Found->second.Primary.Space;
			if (Large && !Found->second.Large && Metrics.LargeObjectCount >= Configuration.MaximumLargeObjects)
				return SpatialRegionStatus::LargeObjectLimit;
			const auto NewMembershipCount = Large ? 0 : MembershipScratch.size();
			if (NewMembershipCount > OldMembershipCount) {
				const auto AdditionalMemberships = NewMembershipCount - OldMembershipCount;
				if (AdditionalMemberships > Configuration.MaximumMemberships ||
					Metrics.MembershipCount > Configuration.MaximumMemberships - AdditionalMemberships)
					return SpatialRegionStatus::MembershipLimit;
			}
			AddedScratch.clear();
			RemovedScratch.clear();
			if (!Large)
				for (const auto Address : MembershipScratch)
					if (!Found->second.Contains(Address)) AddedScratch.push_back(Address);
			if (!Found->second.Large) {
				if (Large) {
					CopyMemberships(Found->second, RemovedScratch);
				} else {
					RemovedScratch.push_back(Found->second.Primary);
					RemovedScratch.insert(
						RemovedScratch.end(), Found->second.Additional.begin(), Found->second.Additional.end()
					);
					std::erase_if(RemovedScratch, [this](SpatialCellAddress Address) {
						return std::ranges::binary_search(MembershipScratch, Address);
					});
				}
			}
			const auto ReclaimableRegions = static_cast<std::size_t>(
				std::ranges::count_if(RemovedScratch, [this](SpatialCellAddress Address) {
					auto Region = Regions.find(Address);
					return Region != Regions.end() && Region->second.size() == 1;
				})
			);
			const auto RetainedRegionCount = Regions.size() - ReclaimableRegions;
			if (NewRegionCount(AddedScratch) > Configuration.MaximumRegions - RetainedRegionCount)
				return SpatialRegionStatus::RegionLimit;
			Entry Candidate;
			try {
				Candidate = MakeEntry(Space, Bounds, Large);
			} catch (const std::bad_alloc &) {
				return SpatialRegionStatus::AllocationFailure;
			}
			bool AddedLarge = false;
			if (Large && (!Found->second.Large || OldSpace != Space)) {
				AddedLarge = InsertLargeObject(Space, Object);
				if (!AddedLarge) return SpatialRegionStatus::AllocationFailure;
			}
			if (!InsertMemberships(Object, AddedScratch)) {
				if (AddedLarge) RemoveLargeObject(Space, Object);
				return SpatialRegionStatus::AllocationFailure;
			}
			RemoveMemberships(Object, RemovedScratch);
			if (Found->second.Large && (!Large || OldSpace != Space)) RemoveLargeObject(OldSpace, Object);
			Candidate.QueryGeneration = Found->second.QueryGeneration;
			Found->second = std::move(Candidate);
			Metrics.MembershipCount -= OldMembershipCount;
			SaturatingIncrement(Metrics.MembershipCount, NewMembershipCount);
			SaturatingIncrement(Metrics.MembershipMoves);
			RefreshGauges();
			return SpatialRegionStatus::Success;
		}

		SpatialRegionStatus Remove(ObjectId Object) {
			auto Found = Objects.find(Object);
			if (Found == Objects.end()) return SpatialRegionStatus::MissingObject;
			CopyMemberships(Found->second, MembershipScratch);
			RemoveMemberships(Object, MembershipScratch);
			if (Found->second.Large) RemoveLargeObject(Found->second.Primary.Space, Object);
			Metrics.MembershipCount -= Found->second.MembershipCount();
			Objects.erase(Found);
			SaturatingIncrement(Metrics.ObjectRemovals);
			RefreshGauges();
			return SpatialRegionStatus::Success;
		}

		SpatialRegionStatus
		Query(std::span<const SpatialRegionQueryVolume> Volumes, SpatialRegionQueryScratch &Scratch) {
			Scratch.Clear();
			SaturatingIncrement(Metrics.RegionQueries);
			auto FailQuery = [this, &Scratch](SpatialRegionStatus Status) {
				Scratch.Clear();
				if (Status == SpatialRegionStatus::QueryVolumeLimit ||
					Status == SpatialRegionStatus::QueryRegionLimit ||
					Status == SpatialRegionStatus::QueryCandidateLimit ||
					Status == SpatialRegionStatus::QueryMembershipVisitLimit)
					SaturatingIncrement(Metrics.QueryLimitFailures);
				if (Status == SpatialRegionStatus::QueryCandidateLimit)
					SaturatingIncrement(Metrics.CandidateLimitFailures);
				return Status;
			};
			if (Volumes.size() > Configuration.MaximumQueryVolumes)
				return FailQuery(SpatialRegionStatus::QueryVolumeLimit);
			if (Volumes.empty()) return SpatialRegionStatus::Success;
			std::array<SpatialCellAddress, MaximumSpatialQueryVolumes> MinimumAddresses;
			std::array<SpatialCellAddress, MaximumSpatialQueryVolumes> MaximumAddresses;
			for (std::size_t VolumeIndex = 0; VolumeIndex < Volumes.size(); ++VolumeIndex) {
				const auto &Volume = Volumes[VolumeIndex];
				if (!Volume.Space.IsValid()) return FailQuery(SpatialRegionStatus::InvalidSpace);
				if (!Volume.IsValid()) return FailQuery(SpatialRegionStatus::InvalidBounds);
				const auto Minimum = SpatialCellAddressForPosition(
					Volume.Center - glm::dvec3(Volume.Radius), Configuration.RegionSize, Volume.Space
				);
				const auto Maximum = SpatialCellAddressForPosition(
					Volume.Center + glm::dvec3(Volume.Radius), Configuration.RegionSize, Volume.Space
				);
				if (!Minimum || !Maximum) return FailQuery(SpatialRegionStatus::InvalidCoordinate);
				MinimumAddresses[VolumeIndex] = *Minimum;
				MaximumAddresses[VolumeIndex] = *Maximum;
				const auto XCount = AxisCount(Minimum->Cell.X, Maximum->Cell.X, Configuration.MaximumQueryRegions);
				const auto YCount = AxisCount(Minimum->Cell.Y, Maximum->Cell.Y, Configuration.MaximumQueryRegions);
				const auto ZCount = AxisCount(Minimum->Cell.Z, Maximum->Cell.Z, Configuration.MaximumQueryRegions);
				if (!XCount || !YCount || !ZCount || *XCount > Configuration.MaximumQueryRegions / *YCount ||
					*XCount * *YCount > Configuration.MaximumQueryRegions / *ZCount)
					return FailQuery(SpatialRegionStatus::QueryRegionLimit);
				const auto VolumeRegionCount = *XCount * *YCount * *ZCount;
				if (VolumeRegionCount > Configuration.MaximumQueryRegions - Scratch.Regions.size())
					return FailQuery(SpatialRegionStatus::QueryRegionLimit);
				for (std::int64_t X = Minimum->Cell.X;; ++X) {
					for (std::int64_t Y = Minimum->Cell.Y;; ++Y) {
						for (std::int64_t Z = Minimum->Cell.Z;; ++Z) {
							Scratch.Regions.push_back({Volume.Space, {X, Y, Z}});
							if (Z == Maximum->Cell.Z) break;
						}
						if (Y == Maximum->Cell.Y) break;
					}
					if (X == Maximum->Cell.X) break;
				}
			}
			if (Volumes.size() > 1) {
				std::ranges::sort(Scratch.Regions);
				auto UniqueEnd = std::ranges::unique(Scratch.Regions).begin();
				Scratch.Regions.erase(UniqueEnd, Scratch.Regions.end());
			}
			if (Scratch.Regions.size() > Configuration.MaximumQueryRegions)
				return FailQuery(SpatialRegionStatus::QueryRegionLimit);
			if (QueryGeneration == std::numeric_limits<std::uint64_t>::max()) {
				for (auto &[Object, Value] : Objects) {
					(void)Object;
					Value.QueryGeneration = 0;
				}
				QueryGeneration = 1;
			} else {
				++QueryGeneration;
				if (QueryGeneration == 0) QueryGeneration = 1;
			}
			std::uint64_t Visits = 0;
			auto AddCandidate = [&](ObjectId Object, bool LargeObject) -> SpatialRegionStatus {
				SaturatingIncrement(Visits);
				SaturatingIncrement(Metrics.CandidateMembershipVisits);
				if (Visits > Configuration.MaximumQueryMembershipVisits)
					return SpatialRegionStatus::QueryMembershipVisitLimit;
				auto Found = Objects.find(Object);
				if (Found == Objects.end()) return SpatialRegionStatus::Success;
				if (Found->second.QueryGeneration == QueryGeneration) {
					SaturatingIncrement(Metrics.CandidateDedupHits);
					return SpatialRegionStatus::Success;
				}
				if (Scratch.Candidates.size() >= Configuration.MaximumQueryCandidates)
					return SpatialRegionStatus::QueryCandidateLimit;
				Found->second.QueryGeneration = QueryGeneration;
				Scratch.Candidates.push_back(Object);
				if (LargeObject) SaturatingIncrement(Metrics.LargeObjectCandidates);
				return SpatialRegionStatus::Success;
			};
			std::array<SpatialSpaceId, MaximumSpatialQueryVolumes> QuerySpaces;
			std::size_t QuerySpaceCount = 0;
			for (const auto &Volume : Volumes) {
				if (std::find(QuerySpaces.begin(), QuerySpaces.begin() + QuerySpaceCount, Volume.Space) !=
					QuerySpaces.begin() + QuerySpaceCount)
					continue;
				QuerySpaces[QuerySpaceCount++] = Volume.Space;
				auto LargeBucket = LargeObjects.find(Volume.Space);
				if (LargeBucket == LargeObjects.end()) continue;
				for (const auto Object : LargeBucket->second)
					if (const auto Status = AddCandidate(Object, true); Status != SpatialRegionStatus::Success)
						return FailQuery(Status);
			}
			SaturatingIncrement(Metrics.RegionsVisited, static_cast<std::uint64_t>(Scratch.Regions.size()));
			if (Volumes.size() == 1) {
				const auto Minimum = MinimumAddresses.front();
				const auto Maximum = MaximumAddresses.front();
				for (std::int64_t X = Minimum.Cell.X;; ++X) {
					for (std::int64_t Y = Minimum.Cell.Y;; ++Y) {
						auto Region = Regions.lower_bound({Minimum.Space, {X, Y, Minimum.Cell.Z}});
						while (Region != Regions.end() && Region->first.Space == Minimum.Space &&
							   Region->first.Cell.X == X && Region->first.Cell.Y == Y &&
							   Region->first.Cell.Z <= Maximum.Cell.Z) {
							for (const auto Object : Region->second)
								if (const auto Status = AddCandidate(Object, false);
									Status != SpatialRegionStatus::Success)
									return FailQuery(Status);
							++Region;
						}
						if (Y == Maximum.Cell.Y) break;
					}
					if (X == Maximum.Cell.X) break;
				}
			} else {
				for (const auto Address : Scratch.Regions) {
					auto Region = Regions.find(Address);
					if (Region == Regions.end()) continue;
					for (const auto Object : Region->second)
						if (const auto Status = AddCandidate(Object, false); Status != SpatialRegionStatus::Success)
							return FailQuery(Status);
				}
			}
			std::ranges::sort(Scratch.Candidates);
			SaturatingIncrement(Metrics.CandidateObjects, Scratch.Candidates.size());
			return SpatialRegionStatus::Success;
		}

		bool VerifyConsistency() const {
			std::size_t MembershipCount = 0;
			for (const auto &[Object, Value] : Objects) {
				if (Value.Large) {
					if (!ContainsLargeObject(Value, Object) || !Value.Primary.IsValid() || Value.MembershipCount() != 0)
						return false;
					continue;
				}
				if (ContainsLargeObject(Value, Object) || !Value.Primary.IsValid()) return false;
				auto Verify = [&](SpatialCellAddress Address) {
					auto Region = Regions.find(Address);
					return Region != Regions.end() && Region->second.contains(Object);
				};
				if (!Verify(Value.Primary)) return false;
				for (const auto Address : Value.Additional)
					if (!Verify(Address)) return false;
				MembershipCount += Value.MembershipCount();
			}
			for (const auto &[Address, Bucket] : Regions) {
				if (Bucket.empty()) return false;
				for (const auto Object : Bucket) {
					auto Found = Objects.find(Object);
					if (Found == Objects.end() || !Found->second.Contains(Address)) return false;
				}
			}
			std::size_t LargeCount = 0;
			for (const auto &[Space, Bucket] : LargeObjects) {
				if (!Space.IsValid() || Bucket.empty()) return false;
				LargeCount += Bucket.size();
				for (const auto Object : Bucket) {
					auto Found = Objects.find(Object);
					if (Found == Objects.end() || !Found->second.Large || Found->second.Primary.Space != Space)
						return false;
				}
			}
			return MembershipCount == Metrics.MembershipCount && Regions.size() == Metrics.RegionCount &&
				   Objects.size() == Metrics.SpatialObjectCount && LargeCount == Metrics.LargeObjectCount;
		}
	};

	SpatialRegionIndex::SpatialRegionIndex(SpatialRegionIndexConfiguration Configuration)
		: State(Configuration.IsValid() ? std::make_unique<Implementation>(Configuration) : nullptr) {
		if (!State) throw std::invalid_argument("[Spatial:RegionIndex] configuration is invalid");
	}

	SpatialRegionIndex::~SpatialRegionIndex() = default;

	SpatialRegionStatus
	SpatialRegionIndex::Register(ObjectId Object, SpatialSpaceId Space, const SpatialBounds &Bounds) {
		return State->Register(Object, Space, Bounds);
	}

	SpatialRegionStatus SpatialRegionIndex::Update(ObjectId Object, SpatialSpaceId Space, const SpatialBounds &Bounds) {
		return State->Update(Object, Space, Bounds);
	}

	SpatialRegionStatus SpatialRegionIndex::Remove(ObjectId Object) {
		return State->Remove(Object);
	}

	SpatialRegionStatus
	SpatialRegionIndex::Query(std::span<const SpatialRegionQueryVolume> Volumes, SpatialRegionQueryScratch &Scratch) {
		try {
			return State->Query(Volumes, Scratch);
		} catch (const std::bad_alloc &) {
			Scratch.Clear();
			return SpatialRegionStatus::AllocationFailure;
		}
	}

	bool SpatialRegionIndex::Contains(ObjectId Object) const {
		return State->Objects.contains(Object);
	}

	bool SpatialRegionIndex::IsLargeObject(ObjectId Object) const {
		auto Found = State->Objects.find(Object);
		return Found != State->Objects.end() && Found->second.Large;
	}

	std::optional<SpatialCellAddress> SpatialRegionIndex::GetPrimaryAddress(ObjectId Object) const {
		auto Found = State->Objects.find(Object);
		return Found == State->Objects.end() ? std::nullopt : std::optional(Found->second.Primary);
	}

	std::size_t SpatialRegionIndex::GetMembershipCount(ObjectId Object) const {
		auto Found = State->Objects.find(Object);
		return Found == State->Objects.end() ? 0 : Found->second.MembershipCount();
	}

	bool SpatialRegionIndex::VerifyConsistency() const {
		return State->VerifyConsistency();
	}

	const SpatialRegionIndexConfiguration &SpatialRegionIndex::GetConfiguration() const {
		return State->Configuration;
	}

	SpatialRegionIndexMetrics SpatialRegionIndex::GetMetrics() const {
		return State->Metrics;
	}
}
