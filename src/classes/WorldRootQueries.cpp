#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"
#include "../terrain/PhysicsStage.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gargantuan {
    namespace {
        glm::vec3 GetTerrainQueryExtents(const PhysicsQueryShape &Shape) {
            switch (Shape.Kind) {
                case PhysicsQueryShapeKind::Sphere: return glm::vec3(Shape.Radius);
                case PhysicsQueryShapeKind::Capsule: return glm::vec3(Shape.Height * 0.5f);
                case PhysicsQueryShapeKind::Box: return glm::vec3(glm::length(Shape.Size) * 0.5f);
            }
            return {};
        }
    }

    WorldRaycastResult WorldRoot::ResolveShapeCast(const PhysicsQueryShape &Shape, glm::vec3 Direction, const RaycastParams &Params) {
        const auto Origin = Shape.Transform.Position;
        if (Params.IncludeTerrain && !HasTerrainQueryCoverage(Origin, Origin + Direction, GetTerrainQueryExtents(Shape)))
            return {.Status = PhysicsOperationStatus::ResourceLimit, .Message = "[Terrain:Query] Collision coverage is unavailable"};
        PhysicsQueryFilter Filter;
        if (auto Prepared=BuildRaycastFilter(Params, Filter); !Prepared.Succeeded())
            return {.Status=Prepared.Status, .Message=std::move(Prepared.Message)};
        return ResolveQueryHit(Physics.ShapeCast({Shape, Direction, std::move(Filter)}), Params.IncludeTerrain);
    }

    WorldRoot::OverlapResult WorldRoot::ResolveOverlap(const PhysicsQueryShape &Shape, const RaycastParams &Params, std::size_t MaxResults) {
        if (MaxResults==0 || MaxResults>MaximumPhysicsOverlapResults)
            return {.Status=PhysicsOperationStatus::InvalidDescription, .Message="MaxResults must be between 1 and 256"};
        if (Params.IncludeTerrain &&
            !HasTerrainQueryCoverage(Shape.Transform.Position, Shape.Transform.Position, GetTerrainQueryExtents(Shape)))
            return {.Status = PhysicsOperationStatus::ResourceLimit, .Message = "[Terrain:Query] Collision coverage is unavailable"};
        PhysicsQueryFilter Filter;
        if (auto Prepared=BuildRaycastFilter(Params, Filter); !Prepared.Succeeded())
            return {.Status=Prepared.Status, .Message=std::move(Prepared.Message)};
        auto BackendResult=Physics.Overlap({Shape, std::move(Filter)});
        if (!BackendResult.Succeeded()) return {.Status=BackendResult.Status, .Message=std::move(BackendResult.Message)};
        OverlapResult Result;
        const auto *TerrainPhysics = GetTerrainPhysics();
        const auto CandidateTerrain = Params.IncludeTerrain ? GetTerrainQueryOwner() : nullptr;
        Result.Parts.reserve(BackendResult.Bodies.size());
        for (auto Id:BackendResult.Bodies) {
            auto Part=ResolvePart(Id);
            if (Part) Result.Parts.push_back(std::move(Part));
            else if (auto Grid = ResolveBlockGrid(Id)) Result.BlockGrids.push_back(std::move(Grid));
            else if (CandidateTerrain && TerrainPhysics->FindOwner(Id))
                Result.TerrainInstance = CandidateTerrain;
            else return {.Status=PhysicsOperationStatus::BackendFailure, .Message="Overlap has no live semantic owner"};
        }
        std::ranges::sort(Result.Parts, {}, [](const auto &Part) { return Part->GetObjectId(); });
        std::ranges::sort(Result.BlockGrids, {}, [](const auto &Grid) { return Grid->GetObjectId(); });
        const auto Repeated = std::ranges::unique(Result.BlockGrids);
        Result.BlockGrids.erase(Repeated.begin(), Repeated.end());
        Result.Truncated = Result.Parts.size() + Result.BlockGrids.size() > MaxResults;
        if (Result.Parts.size() > MaxResults) Result.Parts.resize(MaxResults);
        const auto GridLimit = MaxResults - Result.Parts.size();
        if (Result.BlockGrids.size() > GridLimit) Result.BlockGrids.resize(GridLimit);
        return Result;
    }

    int WorldRoot::RunShapeQuery(lua_State *L, Instance *Value, PhysicsQueryShapeKind Kind, bool IsOverlap) {
        if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::ReadDataModel))
            throw std::runtime_error("Physics world queries require ReadDataModel");
        auto *World=dynamic_cast<WorldRoot *>(Value);
        if (!World || World->GetDestroyed() || World->IsDestroying()) throw std::runtime_error("Query requires a live WorldRoot");
        PhysicsQueryShape Shape;
        Shape.Kind=Kind;
        Shape.Transform=Kind==PhysicsQueryShapeKind::Sphere ? CFrame(CheckStackValue<glm::vec3>(L,2)) : CheckStackValue<CFrame>(L,2);
        int Next=4;
        if (Kind==PhysicsQueryShapeKind::Box) Shape.Size=CheckStackValue<glm::vec3>(L,3);
        else {
            Shape.Radius=CheckStackValue<float>(L,3);
            if (Kind==PhysicsQueryShapeKind::Capsule) { Shape.Height=CheckStackValue<float>(L,4); Next=5; }
        }
        glm::vec3 Direction{};
        if (!IsOverlap) Direction=CheckStackValue<glm::vec3>(L,Next++);
        RaycastParams Params;
        if (!lua_isnoneornil(L,Next)) Params=CheckStackValue<RaycastParams>(L,Next);
        ++Next;
        if (IsOverlap) {
            std::size_t MaxResults=MaximumPhysicsOverlapResults;
            if (!lua_isnoneornil(L,Next)) {
                const double Count=CheckStackValue<double>(L,Next);
                if (!std::isfinite(Count) || Count<1 || Count>MaximumPhysicsOverlapResults || std::floor(Count)!=Count)
                    throw std::runtime_error("MaxResults must be an integer between 1 and 256");
                MaxResults=static_cast<std::size_t>(Count);
            }
            auto Result=World->ResolveOverlap(Shape,Params,MaxResults);
            if (!Result.Succeeded()) throw std::runtime_error(Result.Message);
            lua_createtable(L,0,3);
            lua_createtable(L,static_cast<int>(Result.Parts.size()),0);
            int Index=1;
            for (auto &Part:Result.Parts) {
                StackValue<std::shared_ptr<Instance>>::Push(L,Part);
                lua_rawseti(L,-2,Index++);
            }
            lua_setreadonly(L,-1,true);
            lua_setfield(L,-2,"Parts");
            if (!Result.BlockGrids.empty()) {
                lua_createtable(L, static_cast<int>(Result.BlockGrids.size()), 0);
                int GridIndex = 1;
                for (const auto &Grid : Result.BlockGrids) {
                    StackValue<std::shared_ptr<Instance>>::Push(L, Grid);
                    lua_rawseti(L, -2, GridIndex++);
                }
                lua_setreadonly(L, -1, true);
                lua_setfield(L, -2, "BlockGrids");
            }
            lua_pushboolean(L,Result.Truncated);
            lua_setfield(L,-2,"Truncated");
            if (Result.TerrainInstance) {
                StackValue<std::shared_ptr<Instance>>::Push(L,Result.TerrainInstance);
                lua_setfield(L,-2,"Terrain");
            }
        } else {
            auto Result=World->ResolveShapeCast(Shape,Direction,Params);
            if (!Result.Succeeded()) throw std::runtime_error(Result.Message);
            if (!Result.HasHit()) { lua_pushnil(L); return 1; }
            lua_createtable(L,0,4);
            StackValue<std::shared_ptr<Instance>>::Push(L,Result.GetHitInstance()); lua_setfield(L,-2,"Instance");
            if (Result.Cell) {
                Result.GridInstance->PushGridHit(L, {Result.Cell->x, Result.Cell->y, Result.Cell->z}, Result.Position, Result.Normal);
                lua_setfield(L, -2, "GridHit");
            }
            StackValue<glm::vec3>::Push(L,Result.Position); lua_setfield(L,-2,"Position");
            StackValue<glm::vec3>::Push(L,Result.Normal); lua_setfield(L,-2,"Normal");
            lua_pushnumber(L,Result.Distance); lua_setfield(L,-2,"Distance");
        }
        lua_setreadonly(L,-1,true);
        return 1;
    }
    int WorldRoot::SphereCast(lua_State *L, Instance *Value) { return RunShapeQuery(L,Value,PhysicsQueryShapeKind::Sphere,false); }
    int WorldRoot::CapsuleCast(lua_State *L, Instance *Value) { return RunShapeQuery(L,Value,PhysicsQueryShapeKind::Capsule,false); }
    int WorldRoot::BoxCast(lua_State *L, Instance *Value) { return RunShapeQuery(L,Value,PhysicsQueryShapeKind::Box,false); }
    int WorldRoot::GetPartsInSphere(lua_State *L, Instance *Value) { return RunShapeQuery(L,Value,PhysicsQueryShapeKind::Sphere,true); }
    int WorldRoot::GetPartsInCapsule(lua_State *L, Instance *Value) { return RunShapeQuery(L,Value,PhysicsQueryShapeKind::Capsule,true); }
    int WorldRoot::GetPartsInBox(lua_State *L, Instance *Value) { return RunShapeQuery(L,Value,PhysicsQueryShapeKind::Box,true); }
}
