#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gargantuan {
    WorldRaycastResult WorldRoot::ResolveShapeCast(const PhysicsQueryShape &Shape, glm::vec3 Direction, const RaycastParams &Params) {
        ApplyPendingPhysicsChanges();
        PhysicsQueryFilter Filter;
        if (auto Prepared=BuildRaycastFilter(Params, Filter); !Prepared.Succeeded())
            return {.Status=Prepared.Status, .Message=std::move(Prepared.Message)};
        return ResolveQueryHit(Physics.ShapeCast({Shape, Direction, std::move(Filter)}));
    }

    WorldRoot::OverlapResult WorldRoot::ResolveOverlap(const PhysicsQueryShape &Shape, const RaycastParams &Params, std::size_t MaxResults) {
        if (MaxResults==0 || MaxResults>MaximumPhysicsOverlapResults)
            return {.Status=PhysicsOperationStatus::InvalidDescription, .Message="MaxResults must be between 1 and 256"};
        ApplyPendingPhysicsChanges();
        PhysicsQueryFilter Filter;
        if (auto Prepared=BuildRaycastFilter(Params, Filter); !Prepared.Succeeded())
            return {.Status=Prepared.Status, .Message=std::move(Prepared.Message)};
        auto BackendResult=Physics.Overlap({Shape, std::move(Filter)});
        if (!BackendResult.Succeeded()) return {.Status=BackendResult.Status, .Message=std::move(BackendResult.Message)};
        OverlapResult Result;
        Result.Parts.reserve(BackendResult.Bodies.size());
        for (auto Id:BackendResult.Bodies) {
            auto Part=ResolvePart(Id);
            if (!Part) return {.Status=PhysicsOperationStatus::BackendFailure, .Message="Overlap has no live semantic owner"};
            Result.Parts.push_back(std::move(Part));
        }
        std::ranges::sort(Result.Parts, {}, [](const auto &Part) { return Part->GetObjectId(); });
        Result.Truncated=Result.Parts.size()>MaxResults;
        if (Result.Truncated) Result.Parts.resize(MaxResults);
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
            lua_createtable(L,0,2);
            lua_createtable(L,static_cast<int>(Result.Parts.size()),0);
            int Index=1;
            for (auto &Part:Result.Parts) {
                StackValue<std::shared_ptr<Instance>>::Push(L,Part);
                lua_rawseti(L,-2,Index++);
            }
            lua_setreadonly(L,-1,true);
            lua_setfield(L,-2,"Parts");
            lua_pushboolean(L,Result.Truncated);
            lua_setfield(L,-2,"Truncated");
        } else {
            auto Result=World->ResolveShapeCast(Shape,Direction,Params);
            if (!Result.Succeeded()) throw std::runtime_error(Result.Message);
            if (!Result.HasHit()) { lua_pushnil(L); return 1; }
            lua_createtable(L,0,4);
            StackValue<std::shared_ptr<Instance>>::Push(L,Result.Instance); lua_setfield(L,-2,"Instance");
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
