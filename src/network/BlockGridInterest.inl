// Included inside GameSession::Impl. Current and staged interests both come from server-owned state.
GridInterest GetGridInterest(ConnectionId Connection,const Peer& Value,bool Staged) const {
	GridInterest Result;
	if (Configuration.Role!=GameSessionRole::Server) return Result;
	if (Staged) {
		if (Value.Staging) Result.Destination=Value.Staging->Frame.Bounds;
		return Result;
	}
	if (Relevance) {
		const auto Focus=Relevance->GetResolvedFocus(Connection);
		Result.Count=std::min(Result.Centers.size(),Focus.size());
		std::copy_n(Focus.begin(),Result.Count,Result.Centers.begin());
	}
	if (!Result.Count) {
		Result.Centers[0]=GetTerrainFocusBounds(TerrainFocusActor(Value.PlayerValue)).Position;
		Result.Count=1;
	}
	return Result;
}
