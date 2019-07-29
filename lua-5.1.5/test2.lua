function fa()
	local i = 1
	return function()
		i = i + 1
		return i
	end
end
test1 = fa()
test2 = fa()
print(test1())
print(test1())
print(test2())
print(test2())
