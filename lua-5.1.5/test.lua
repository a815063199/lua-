--[[function fa()
	local i = 0
	return function()
		i = i + 1
		return i
	end
end
test1 = fa()
test2 = fa()
test1()
test1()
test2()
test2()]]
local upvalue_2 = 1
function fa()
	local upvalue_1 = 0
	return function()
		upvalue_1 = upvalue_1 + upvalue_2
		return upvalue_1
	end
end
test1 = fa()
test2 = fa()
print('************', test1())
print('************', test1())
print('************', test2())
print('************', test2())
