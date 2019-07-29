function fa()
	local a = 0
	return function()
		local b = 0
		return function()
			a = a + 1
			b = b + 1
			return a, b
		end
	end
end

fb_1 = fa()
fc_1 = fb_1()
print(fc_1())
print(fc_1())
print(fc_1())
print(fc_1())
print(fc_1())
print('-------------------------')
fb_2 = fa()
fc_2 = fb_2()
print(fc_2())
print(fc_2())
print(fc_2())
print(fc_2())
print(fc_2())
