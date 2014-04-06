print "check that only functions with same upvalues are same"
local upv=1
local function x() return function() upv=upv+1 return upv end end
assert(x()==x());x()();x()()
assert(upv==3)
local function x() local z; return function() upv=upv+1 return upv+z end end
assert(x()~=x())
print 'ok'
