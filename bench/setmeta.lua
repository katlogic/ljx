local setmetatable = require '__gc'
local count=0
local m = {
  __gc = function() count = count+1 end
}

local res={}
local tabs={}
for i=1,1000000 do
  table.insert(tabs, setmetatable({},m))
end
tabs = nil
