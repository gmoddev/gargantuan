local V = Vector3.new
local X,Y,Z = V(1,0,0),V(0,1,0),V(0,0,1)
local Faces = {{"+X",X},{"-X",-X},{"+Y",Y},{"-Y",-Y},{"+Z",Z},{"-Z",-Z}}
local Room = Instance.new("SpatialRegion")
Room.Parent = workspace
Room.Gravity = 0
local Positions = {V(-2,1,3),V(3,-1,-2)}
local Unit = .5
local CubeMass = Unit^3
local CubeCenter = (Positions[2]+V(.5,.5,.5))*Unit
local Count, MaximumCenterError, MaximumInertiaError = 0, 0, 0

for _,Spec in {
	{"Slab",.5,V(.5,.25,.5),1/48,1/12,0},
	{"Wedge",.5,V(.5,1/3,2/3),1/18,1/18,1/36},
	{"Stairs",.75,V(.5,5/12,7/12),11/144,11/144,1/36},
} do
	local G=Instance.new("BlockGrid")
	G.CellSize=Unit
	G.Parent=Room
	G:DefineBlock("project:mass",{Shape=Spec[1]})
	for _,Up in Faces do
		for _,Front in Faces do
			local U,F=Up[2],Front[2]
			if U:Dot(F)~=0 then continue end
			local Right,Back=F:Cross(U),-F
			local function Rotate(A) return Right*A.X+U*A.Y+Back*A.Z end
			local Mass=CubeMass*Spec[2]
			local Center=(Positions[1]+V(.5,.5,.5)+Rotate(Spec[3]-V(.5,.5,.5)))*Unit
			G:SetBlock(Positions[1],"project:mass",{Up=Up[1],Front=Front[1]})
			if G.MotionMode~="Dynamic" then G:SetMotionMode("Dynamic",G:GetRevision()) end
			local function Verify(WithCube)
				local S=G:GetAssemblyState()
				local CombinedMass=if WithCube then Mass+CubeMass else Mass
				local COM=if WithCube then (Center*Mass+CubeCenter*CubeMass)/CombinedMass else Center
				assert(math.abs(S.Mass-CombinedMass)<1e-5 and S.Mode=="Dynamic","incorrect volume for "..Spec[1])
				local CenterError=(S.LocalCenterOfMass-COM).Magnitude
				MaximumCenterError=math.max(MaximumCenterError,CenterError)
				assert(CenterError<.0001,"incorrect first moment for "..Spec[1].." "..Up[1]..Front[1])
				for Column,Axis in {X,Y,Z} do
					local A=V(Right:Dot(Axis),U:Dot(Axis),Back:Dot(Axis))
					local Cov=V(A.X/12,Spec[4]*A.Y+Spec[6]*A.Z,Spec[6]*A.Y+Spec[5]*A.Z)
					local Inertia=Rotate(A*(1/12+Spec[4]+Spec[5])-Cov)*(Mass*Unit^2)
					if WithCube then
						local R1,R2=Center-COM,CubeCenter-COM
						Inertia+=Mass*(R1:Dot(R1)*Axis-R1*R1:Dot(Axis))+
							CubeMass*((Unit^2/6+R2:Dot(R2))*Axis-R2*R2:Dot(Axis))
					end
					local Error=(S.LocalInertia[Column]-Inertia).Magnitude
					MaximumInertiaError=math.max(MaximumInertiaError,Error)
					assert(Error<.0002,"incorrect tensor for "..Spec[1].." "..Up[1]..Front[1].." "..tostring(Error))
				end
				Count+=1
			end
			Verify(false)
			G:SetBlock(Positions[2],"Brick")
			Verify(true)
			G:RemoveBlock(Positions[2])
			Verify(false)
		end
	end
	G:Destroy()
end
Room:Destroy()
assert(Count==216)
print(string.format("[Spatial:CompoundMass] comparisons=%d maxCenterError=%.8f maxInertiaError=%.8f",Count,MaximumCenterError,MaximumInertiaError))
