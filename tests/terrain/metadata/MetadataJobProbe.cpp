// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/terrain/ActiveChunkRegistry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
    using gargantuan::ExecutionDomain;
    using gargantuan::GetCurrentExecutionDomain;
    using gargantuan::JobGroup;
    using gargantuan::JobSystem;
    using gargantuan::terrain::ChunkKey;
    using gargantuan::terrain::detail::ActiveChunkRegistry;
    using gargantuan::terrain::detail::ChunkWorkTicket;

    using Clock = std::chrono::steady_clock;
    constexpr std::size_t TicketCount = 64;
    constexpr std::size_t SampleCount = 101;

    void Require(bool Condition) {
        if (!Condition) {
            std::cerr << "[Terrain:Metadata] worker qualification failed\n";
            std::exit(2);
        }
    }

    double Micros(Clock::time_point Start, Clock::time_point Stop) {
        return std::chrono::duration<double, std::micro>(Stop - Start).count();
    }

    struct Product {
        std::uint64_t Data = 0;
        ExecutionDomain Domain = ExecutionDomain::Main;
    };

    void Run(std::size_t WorkerCount, std::size_t BatchSize) {
        JobSystem Jobs(WorkerCount);
        ActiveChunkRegistry Registry(1024, 1);
        for (std::int64_t X = -512; X < 512; ++X)
            Require(Registry.Activate({X, 0, 0}).has_value());

        constexpr std::array Labels{"Capture", "Submit", "Wait", "MainAdmission", "Total"};
        std::array<std::array<double, SampleCount>, Labels.size()> Times{};
        for (std::size_t Pass = 0; Pass <= SampleCount; ++Pass) {
            auto Group = std::make_shared<JobGroup>();
            std::array<ChunkWorkTicket, TicketCount> Tickets{};
            std::array<Product, TicketCount> Products{};
            const auto Start = Clock::now();
            for (std::size_t I = 0; I < Tickets.size(); ++I) {
                const ChunkKey Key{static_cast<std::int64_t>((I + Pass * TicketCount) % 1024) - 512, 0, 0};
                const auto Ticket = Registry.Capture(Key);
                Require(Ticket.has_value());
                Tickets[I] = *Ticket;
            }
            const auto Captured = Clock::now();

            // One bounded wave. Each callback writes only its disjoint output
            // cells. Tickets are immutable until the group joins.
            for (std::size_t From = 0; From < Tickets.size(); From += BatchSize) {
                Jobs.Submit([&Tickets, &Products, From, BatchSize] {
                    for (std::size_t I = From; I < From + BatchSize; ++I) {
                        const ChunkWorkTicket &Ticket = Tickets[I];
                        Products[I] = {
                            std::rotl(Ticket.Generation + Ticket.Version.Geometry, static_cast<int>(I)),
                            GetCurrentExecutionDomain(),
                        };
                    }
                }, Group);
            }
            const auto Submitted = Clock::now();

            for (std::size_t I = 0; I < Tickets.size(); I += 8) {
                const auto &Prior = Tickets[I];
                Require(Registry.Update(Prior.Key, {Prior.Version.Geometry + 1, Prior.Version.Material}));
            }
            Group->Wait();
            const auto Waited = Clock::now();

            Require(Group->GetFirstException() == nullptr);
            std::size_t Accepted = 0;
            for (std::size_t I = 0; I < Tickets.size(); ++I) {
                Require(Products[I].Domain == ExecutionDomain::Worker);
                Require(Products[I].Data == std::rotl(Tickets[I].Generation + Tickets[I].Version.Geometry,
                        static_cast<int>(I)));
                const bool Current = Registry.IsCurrent(Tickets[I]);
                Require(Current == (I % 8 != 0));
                Accepted += Current;
            }
            Require(Accepted == 56 && GetCurrentExecutionDomain() == ExecutionDomain::Main);
            const auto Finished = Clock::now();
            if (Pass != 0) {
                Times[0][Pass - 1] = Micros(Start, Captured);
                Times[1][Pass - 1] = Micros(Captured, Submitted);
                Times[2][Pass - 1] = Micros(Submitted, Waited);
                Times[3][Pass - 1] = Micros(Waited, Finished);
                Times[4][Pass - 1] = Micros(Start, Finished);
            }
        }
        Jobs.Drain();

        for (std::size_t I = 0; I < Labels.size(); ++I) {
            auto &Samples = Times[I];
            std::ranges::sort(Samples);
            const auto Percentile = [&](std::size_t Rank) { return Samples[(Rank * Samples.size() + 99) / 100 - 1]; };
            std::cout << WorkerCount << ',' << BatchSize << ',' << Labels[I] << ',' << Samples.size() << ','
                      << Percentile(50) << ',' << Percentile(95) << ',' << Percentile(99) << ',' << Samples.back() << '\n';
        }
    }
}

int main() {
    std::cout << "Workers,TicketsPerJob,Phase,Samples,P50_us,P95_us,P99_us,Worst_us\n";
    for (const auto WorkerCount : {1u, 2u}) {
        Run(WorkerCount, 1);
        Run(WorkerCount, 8);
    }
}
