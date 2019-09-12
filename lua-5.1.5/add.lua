local test_lib = require 'test_lib'
local sum = test_lib.add(1, 2)
print("sum = ", sum)
local pos = test_lib.str_find("asdfdsf", "dsf");
print("pos = ", pos)
local str = test_lib.str_concat("abc", "def")
print("str = ", str)
