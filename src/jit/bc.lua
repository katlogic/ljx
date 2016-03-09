----------------------------------------------------------------------------
-- LuaJIT bytecode listing module.
--
-- Copyright (C) 2005-2016 Mike Pall. All rights reserved.
-- Released under the MIT license. See Copyright Notice in luajit.h
----------------------------------------------------------------------------
--
-- This module lists the bytecode of a Lua function. If it's loaded by -jbc
-- it hooks into the parser and lists all functions of a chunk as they
-- are parsed.
--
-- Example usage:
--
--   luajit -jbc -e 'local x=0; for i=1,1e6 do x=x+i end; print(x)'
--   luajit -jbc=- foo.lua
--   luajit -jbc=foo.list foo.lua
--
-- Default output is to stderr. To redirect the output to a file, pass a
-- filename as an argument (use '-' for stdout) or set the environment
-- variable LUAJIT_LISTFILE. The file is overwritten every time the module
-- is started.
--
-- This module can also be used programmatically:
--
--   local bc = require("jit.bc")
--
--   local function foo() print("hello") end
--
--   bc.dump(foo)           --> -- BYTECODE -- [...]
--   print(bc.line(foo, 2)) --> 0002    KSTR     1   1      ; "hello"
--
--   local out = {
--     -- Do something with each line:
--     write = function(t, ...) io.write(...) end,
--     close = function(t) end,
--     flush = function(t) end,
--   }
--   bc.dump(foo, out)
--
------------------------------------------------------------------------------

-- Cache some library functions and objects.
local jit = require("jit")
assert(jit.version_num == 20100, "LuaJIT core/library version mismatch")
local jutil = require("jit.util")
local vmdef = require("jit.vmdef")
local bit = require("bit")
local sub, gsub, format = string.sub, string.gsub, string.format
local byte, band, shr = string.byte, bit.band, bit.rshift
local funcinfo, funcbc, funck = jutil.funcinfo, jutil.funcbc, jutil.funck
local funcuvname = jutil.funcuvname
local bcnames = vmdef.bcnames
local stdout, stderr = io.stdout, io.stderr

------------------------------------------------------------------------------

local function ctlsub(c)
  if c == "\n" then return "\\n"
  elseif c == "\r" then return "\\r"
  elseif c == "\t" then return "\\t"
  else return format("\\%03d", byte(c))
  end
end

-- Return one bytecode line.
local function bcline(func, pc, prefix)
  local ins, m = funcbc(func, pc)
  if not ins then return end
  local ma, mb, mc = band(m, 7), band(m, 15*8), band(m, 15*128)
  local a = band(shr(ins, 8), 0xff)
  local oidx = 6*band(ins, 0xff)
  local op = sub(bcnames, oidx+1, oidx+6)
  local s = format("%04d %s %-6s %3s ",
    pc, prefix or "  ", op, ma == 0 and "" or a)
  local d = shr(ins, 16)
  if mc == 13*128 then -- BCMjump
    return format("%s=> %04d\n", s, pc+d-0x7fff)
  end
  if mb ~= 0 then
    d = band(d, 0xff)
  elseif mc == 0 then
    return s.."\n"
  end
  local kc
  if mc == 10*128 then -- BCMstr
    kc = funck(func, -d-1)
    kc = format(#kc > 40 and '"%.40s"~' or '"%s"', gsub(kc, "%c", ctlsub))
  elseif mc == 9*128 then -- BCMnum
    kc = funck(func, d)
    if op == "TSETM " then kc = kc - 2^52 end
  elseif mc == 12*128 then -- BCMfunc
    local fi = funcinfo(funck(func, -d-1))
    if fi.ffid then
      kc = vmdef.ffnames[fi.ffid]
    else
      kc = fi.loc
    end
  elseif mc == 5*128 then -- BCMuv
    kc = funcuvname(func, d)
  end
  if ma == 5 then -- BCMuv
    local ka = funcuvname(func, a)
    if kc then kc = ka.." ; "..kc else kc = ka end
  end
  if mb ~= 0 then
    local b = shr(ins, 24)
    if kc then return format("%s%3d %3d  ; %s\n", s, b, d, kc) end
    return format("%s%3d %3d\n", s, b, d)
  end
  if kc then return format("%s%3d      ; %s\n", s, d, kc) end
  if mc == 7*128 and d > 32767 then d = d - 65536 end -- BCMlits
  return format("%s%3d\n", s, d)
end

-- Collect branch targets of a function.
local function bctargets(func)
  local target = {}
  for pc=1,1000000000 do
    local ins, m = funcbc(func, pc)
    if not ins then break end
    if band(m, 15*128) == 13*128 then target[pc+shr(ins, 16)-0x7fff] = true end
  end
  return target
end

-- Dump bytecode instructions of a function.
local function bcdump(func, out, all, prefix)
  prefix = prefix or ""
  if not out then out = stdout end
  local fi = funcinfo(func)
  if all and fi.children then
    for n=-1,-1000000000,-1 do
      local k = funck(func, n)
      if not k then break end
      if type(k) == "proto" then bcdump(k, out, true) end
    end
  end
  out:write(format(prefix.."+- BYTECODE -- %s-%d\n", fi.loc, fi.lastlinedefined))
  for i=1,(#fi.uvinit) do
    local uvv = band(fi.uvinit[i],0xfff) -- ORDER PROTO_UV_* in lj_obj.h
    local loc = band(fi.uvinit[i],0x8000)~=0 and " PARENT" or ""
    local imu = band(fi.uvinit[i],0x4000)~=0 and " IMMUTABLE" or ""
    local env = band(fi.uvinit[i],0x2000)~=0 and " ENV" or ""
    local clo = band(fi.uvinit[i],0x1000)~=0 and " CLOSURE" or ""
    out:write((prefix.."| UV@%d = %d%s%s%s%s %04x\n"):format(i-1,uvv,loc,imu,env,clo,fi.uvinit[i]))
    if clo ~= "" then
      local k = funck(func, -band(fi.uvinit[i],0xfff)-1)
      out:write(prefix .. "|   |\n")
      if k then
        bcdump(k, out, nil, prefix .. "|   ")
      else
        print ("cant load k",-band(fi.uvinit[i],0xfff)-1)
      end
    end
  end
  out:write(prefix.."+-----------\n")

  local target = bctargets(func)
  for pc=1,1000000000 do
    local s = bcline(func, pc, target[pc] and "=>")
    if not s then break end
    out:write(prefix .. s)
  end
  out:write(prefix.."\n")
  out:flush()
end

------------------------------------------------------------------------------

-- Active flag and output file handle.
local active, out

-- List handler.
local function h_list(func)
  return bcdump(func, out)
end

-- Detach list handler.
local function bclistoff()
  if active then
    active = false
    jit.attach(h_list)
    if out and out ~= stdout and out ~= stderr then out:close() end
    out = nil
  end
end

-- Open the output file and attach list handler.
local function bcliston(outfile)
  if active then bclistoff() end
  if not outfile then outfile = os.getenv("LUAJIT_LISTFILE") end
  if outfile then
    out = outfile == "-" and stdout or assert(io.open(outfile, "w"))
  else
    out = stderr
  end
  jit.attach(h_list, "bc")
  active = true
end

-- Public module functions.
return {
  line = bcline,
  dump = bcdump,
  targets = bctargets,
  on = bcliston,
  off = bclistoff,
  start = bcliston -- For -j command line option.
}

