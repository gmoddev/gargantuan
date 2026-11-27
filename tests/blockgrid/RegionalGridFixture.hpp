// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"
#include <stdexcept>

namespace gargantuan::blockgrid::test {
	inline Field LargePublicField() {
		constexpr unsigned Width=12,Height=8,Depth=8;
		std::vector<std::uint8_t> Bytes{'B','G','R','D',1};
		auto Put=[&](unsigned Value,unsigned N) {
			for (unsigned I=0;I<N;++I) Bytes.push_back(static_cast<std::uint8_t>(Value>>(8*I)));
		};
		Put(Width*Height*Depth,4);
		for (unsigned Z=0;Z<Depth;++Z) for (unsigned Y=0;Y<Height;++Y) for (unsigned X=0;X<Width;++X) {
			Put(X*LeafSide,4);Put(Y*LeafSide,4);Put(Z*LeafSide,4);Put(2,1);
			for (unsigned I=0;I<LeafVolume;++I) {
				Put(1+(I+X+Y+Z)%2,2);Put((I+X+Y+Z)%24,1);
			}
		}
		auto Grid=Decode(Bytes);
		if (Encode(Grid)!=Bytes || !Grid.Define({"project:distant",Shape::Slab}) ||
			!Grid.Define({"project:near",Shape::Wedge}) ||
			!Grid.DefineChannel({"private_score",ChannelKind::UInt8,0,true,false,false,false}) ||
			!Grid.DefineChannel({"charge",ChannelKind::UInt8,0,true,true,true,true}))
			throw std::runtime_error("[BlockGrid:Fixture] Large native field initialization failed");
		const std::array<Operation,4> Edits{Write{SingleCell({84,52,52}),{3,9}},
			Write{SingleCell({12,12,12}),{4,11}},ValueWrite{{12,12,12},0,231},ValueWrite{{12,12,12},1,17}};
		if (!Grid.ApplyOperations(Edits)) throw std::runtime_error("[BlockGrid:Fixture] Large field data was unchanged");
		return Grid;
	}
}
