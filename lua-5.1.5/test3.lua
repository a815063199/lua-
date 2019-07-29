function fa()
	local i = 0
	return function()
		i = i + 1
		return i
	end
end
test1 = fa()
print(test1())
print(test1())
print('-----------')
test2 = fa()
print(test2())
print(test2())
