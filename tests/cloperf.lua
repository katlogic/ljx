-- benchmark closure/upvalue performance

local upv=4
local dummy
local function cc(n)
  local res
  do
	dummy = function() return dummy end
	local za=upv+4
	res=za+1
  end
  return res+upv+1+n
end

local function x()
  return cc
end

res=0
for i=1,1e8 do
	local r = x()
	res=res+r(i)
end
