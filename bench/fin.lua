local setmetatable = require "__gc"
r={}
tm={}
count=0
tm.__gc=function(t)
	count=count+1
	if exiting then
		assert(count==3)
		print "looking good"
	end
	r=t -- resurrect
	setmetatable(t, tm) -- and tell gc it resurrected
end
setmetatable(r, tm)
r=nil
collectgarbage("collect")
r=nil
collectgarbage("collect")
assert(count==2)
exiting=1
--os.exit()
