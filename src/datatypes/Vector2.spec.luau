local Lest = require("@lest")
local describe, it, expect = Lest.describe, Lest.it, Lest.expect

local function near(a: number, b: number, epsilon: number?)
	epsilon = epsilon or 1e-5
	return math.abs(a - b) <= epsilon
end

local function expectVecNear(v: Vector2, x: number, y: number, epsilon: number?)
	expect(near(v.X, x, epsilon)).toBeTruthy()
	expect(near(v.Y, y, epsilon)).toBeTruthy()
end

describe("Vector2", function()
	it("defaults to zero", function()
		local v = Vector2.new()
		expect(v.X).toBe(0)
		expect(v.Y).toBe(0)
	end)

	it("constructs with arguments", function()
		local v = Vector2.new(6, 7)
		expect(v.X).toBe(6)
		expect(v.Y).toBe(7)
	end)

	it("equals", function()
		expect(Vector2.new(1, 2)).toEqual(Vector2.new(1, 2))
		expect(Vector2.new(1, 2)).toNotEqual(Vector2.new(1, 3))
		expect(Vector2.new(1, 2)).toNotEqual(Vector2.new(3, 2))
	end)

	it("stringifies", function()
		expect(tostring(Vector2.new(6, 7))).toBe("6, 7")
	end)

	it("returns correct magnitude", function()
		local v = Vector2.new(3, 4)
		expect(v.Magnitude).toBe(5)
	end)

	it("normalizes Unit and preserves direction", function()
		expectVecNear(Vector2.new(5, 5).Unit, math.sqrt(0.5), math.sqrt(0.5))
	end)

	it("returns zero unit vector for zero vector", function()
		expect(Vector2.new(0, 0).Unit).toEqual(Vector2.new(0, 0))
	end)

	it("returns correct dot product", function()
		local a = Vector2.new(1, 2)
		local b = Vector2.new(3, 4)
		expect(a:Dot(b)).toBe(11)
	end)

	it("applies the complete arithmetic operator matrix", function()
		local a = Vector2.new(1, 2)
		local b = Vector2.new(3, 4)
		expectVecNear(a + b, 4, 6)
		expectVecNear(b - a, 2, 2)
		expectVecNear(-a, -1, -2)
		expectVecNear(a * b, 3, 8)
		expectVecNear(a * 2, 2, 4)
		expectVecNear(2 * a, 2, 4)
		expectVecNear(Vector2.new(8, 12) / Vector2.new(2, 3), 4, 4)
		expectVecNear(b / 2, 1.5, 2)
	end)

	it("rejects unsupported arithmetic operands without crashing", function()
		local invalidOperations = {
			function()
				return Vector2.new(1, 2) + 2
			end,
			function()
				return 2 + Vector2.new(1, 2)
			end,
			function()
				return Vector2.new(1, 2) * "x"
			end,
			function()
				return "hello" * Vector2.new(1, 2)
			end,
			function()
				return Vector2.new(1, 2) / nil
			end,
			function()
				return 2 / Vector2.new(1, 2)
			end,
			function()
				return Vector2.new(1, 2) + UDim.new(1, 2)
			end,
			function()
				return 2 + UDim.new(1, 2)
			end,
		}

		for _, operation in invalidOperations do
			local ok = pcall(operation)
			expect(ok).toBeFalsy()
		end
	end)

	-- it("Distance and DistanceSquared", function()
	-- 	local a = Vector2.new(1, 2, 3)
	-- 	local b = Vector2.new(4, 6, 3)
	-- 	expect(a:Distance(b)).toBe(5)
	-- 	expect(a:DistanceSquared(b)).toBe(25)
	-- end)

	it("lerps midpoint", function()
		local a = Vector2.new(0, 0)
		local b = Vector2.new(10, 20)
		local v = a:Lerp(b, 0.5)
		expectVecNear(v, 5, 10)
	end)

	-- it("ClampMagnitude", function()
	-- 	local big = Vector2.new(3, 4, 0):ClampMagnitude(2)
	-- 	local small = Vector2.new(1, 1, 0):ClampMagnitude(10)
	-- 	expect(near(big:Magnitude(), 2)).toBeTruthy()
	-- 	expectVecNear(small, 1, 1, 0)
	-- end)

	-- it("RotationMatrix + RotateByMatrix", function()
	-- 	local axis = Vector2.new(0, 0, 1)
	-- 	local m = Vector2.RotationMatrix(axis, math.pi / 2)
	-- 	local v = Vector2.new(1, 0, 0):RotateByMatrix(m)
	-- 	expectVecNear(v, 0, 1, 0)
	-- end)

	-- it("RotateAroundAxis", function()
	-- 	local v = Vector2.new(1, 0, 0):RotateAroundAxis(Vector2.new(0, 0, 1), math.pi / 2)
	-- 	expectVecNear(v, 0, 1, 0)
	-- end)

	-- it("Project", function()
	-- 	local v = Vector2.new(3, 4, 0):Project(Vector2.new(1, 0, 0))
	-- 	expectVecNear(v, 3, 0, 0)
	-- end)

	-- it("returns correct Angle", function()
	-- 	local x = Vector2.new(1, 0, 0)
	-- 	local y = Vector2.new(0, 1, 0)
	-- 	expect(near(x:Angle(y), math.pi / 2)).toBeTruthy()
	-- end)

	-- it("Reflect", function()
	-- 	local v = Vector2.new(1, -1, 0)
	-- 	local normal = Vector2.new(0, 1, 0)
	-- 	local r = v:Reflect(normal)
	-- 	expectVecNear(r, 1, 1, 0)
	-- end)

	it("returns correct scalar and component utility values", function()
		local v = Vector2.new(-1.49, 2.5)
		expectVecNear(v:Abs(), 1.49, 2.5)
		expectVecNear(v:Floor(), -2, 2)
		expectVecNear(v:Ceil(), -1, 3)
		expectVecNear(Vector2.new(-10, 0):Sign(), -1, 0)
		expectVecNear(v:Min(Vector2.new(-2, 3)), -2, 2.5)
		expectVecNear(v:Max(Vector2.new(-2, 3)), -1.49, 3)
		expect(Vector2.new(1, 2):Cross(Vector2.new(3, 4))).toBe(-2)
		expect(Vector2.new(1, 2):FuzzyEq(Vector2.new(1.00001, 1.99999), 0.0001)).toBeTruthy()
		expect(Vector2.new(1, 2):FuzzyEq(Vector2.new(1.001, 2), 0.0001)).toBeFalsy()
	end)

	it("validates method arguments and FuzzyEq epsilon", function()
		local invalidCalls = {
			function()
				return Vector2.new(1, 2):Dot(UDim.new())
			end,
			function()
				return Vector2.new(1, 2):Cross("wrong")
			end,
			function()
				return Vector2.new(1, 2):Lerp(Vector2.zero, "wrong")
			end,
			function()
				return Vector2.new(1, 2):FuzzyEq("wrong")
			end,
			function()
				return Vector2.new(1, 2):FuzzyEq(Vector2.zero, -1)
			end,
			function()
				return Vector2.new(1, 2):FuzzyEq(Vector2.zero, math.huge)
			end,
		}

		for _, call in invalidCalls do
			local ok = pcall(call)
			expect(ok).toBeFalsy()
		end
	end)

	-- it("DirectionTo and DirectionToLength", function()
	-- 	local from = Vector2.new(1, 1, 1)
	-- 	local to = Vector2.new(4, 1, 1)
	-- 	expectVecNear(from:DirectionTo(to), 1, 0, 0)
	-- 	expectVecNear(from:DirectionToLength(to, 2), 2, 0, 0)
	-- end)

	-- it("LookAt basis vectors", function()
	-- 	local basis = Vector2.new(0, 0, 0):LookAt(Vector2.new(0, 0, 1))
	-- 	expectVecNear(basis.forward, 0, 0, 1)
	-- 	expectVecNear(basis.right, 1, 0, 0)
	-- 	expectVecNear(basis.up, 0, 1, 0)
	-- end)

	-- it("IsZero and Equals", function()
	-- 	local almostZero = Vector2.new(1e-6, -1e-6, 1e-6)
	-- 	expect(almostZero:IsZero()).toBeTruthy()
	-- 	expect(Vector2.new(1, 2, 3):Equals(Vector2.new(1.000001, 2.000001, 3.000001), 1e-3)).toBeTruthy()
	-- end)

	-- it("Slerp halfway between orthogonal unit vectors", function()
	-- 	local a = Vector2.new(1, 0, 0)
	-- 	local b = Vector2.new(0, 1, 0)
	-- 	local c = Vector2.Slerp(a, b, 0.5)
	-- 	local s = math.sqrt(0.5)
	-- 	expectVecNear(c, s, s, 0)
	-- end)

	-- it("ToTable and FromTable roundtrip", function()
	-- 	local a = Vector2.new(7, 8, 9)
	-- 	local tbl = a:ToTable()
	-- 	local b = Vector2.FromTable(tbl)
	-- 	expectVecNear(b, 7, 8, 9)
	-- end)

	it("has immutable value constants", function()
		expectVecNear(Vector2.zero, 0, 0)
		expectVecNear(Vector2.one, 1, 1)
		expectVecNear(Vector2.xAxis, 1, 0)
		expectVecNear(Vector2.yAxis, 0, 1)
		local ok = pcall(function()
			Vector2.zero = Vector2.one
		end)
		expect(ok).toBeFalsy()
		expectVecNear(Vector2.zero, 0, 0)
	end)

	it("does not expose ordering operators", function()
		local operators = {
			function()
				return Vector2.zero < Vector2.one
			end,
			function()
				return Vector2.zero <= Vector2.one
			end,
			function()
				return Vector2.one > Vector2.zero
			end,
			function()
				return Vector2.one >= Vector2.zero
			end,
		}
		for _, operation in operators do
			local ok = pcall(operation)
			expect(ok).toBeFalsy()
		end
	end)
end)
