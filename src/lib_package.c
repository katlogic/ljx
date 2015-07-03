/*
** Package library.
** Copyright (C) 2005-2015 Mike Pall. See Copyright Notice in luajit.h
**
** Lua 5.2 port
** Copyright (C) 2014 Karel Tuma. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua 5.2 interpreter.
** Copyright (C) 1994-2014 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_package_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_lib.h"

/* ------------------------------------------------------------------------ */
#define LUA_PATHSUFFIX		"_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR
#define LUA_PATHVERSION		LUA_PATH LUA_PATHSUFFIX
#define LUA_CPATHVERSION	LUA_CPATH LUA_PATHSUFFIX
#define CLIBS		"_CLIBS"
#if !defined(LUA_CSUBSEP)
#define LUA_CSUBSEP		LUA_DIRSEP
#endif

#if !defined(LUA_LSUBSEP)
#define LUA_LSUBSEP		LUA_DIRSEP
#endif



/* Error codes for ll_loadfunc. */
#define PACKAGE_ERR_LIB		1
#define PACKAGE_ERR_FUNC	2
#define PACKAGE_ERR_LOAD	3

/* Redefined in platform specific part. */
#define PACKAGE_LIB_FAIL	"open"
#define setprogdir(L)		((void)0)

/* Symbol name prefixes. */
#define SYMPREFIX_CF		"luaopen_%s"
#define SYMPREFIX_BC		"luaJIT_BC_%s"

#if LJ_TARGET_DLOPEN

#include <dlfcn.h>

static void ll_unloadlib(void *lib)
{
  dlclose(lib);
}

static void *ll_load(lua_State *L, const char *path, int gl)
{
  void *lib = dlopen(path, RTLD_NOW | (gl ? RTLD_GLOBAL : RTLD_LOCAL));
  if (lib == NULL) lua_pushstring(L, dlerror());
  return lib;
}

static lua_CFunction ll_sym(lua_State *L, void *lib, const char *sym)
{
  lua_CFunction f = (lua_CFunction)dlsym(lib, sym);
  if (f == NULL) lua_pushstring(L, dlerror());
  return f;
}

static const char *ll_bcsym(void *lib, const char *sym)
{
#if defined(RTLD_DEFAULT)
  if (lib == NULL) lib = RTLD_DEFAULT;
#elif LJ_TARGET_OSX || LJ_TARGET_BSD
  if (lib == NULL) lib = (void *)(intptr_t)-2;
#endif
  return (const char *)dlsym(lib, sym);
}

#elif LJ_TARGET_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS  4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT  2
BOOL WINAPI GetModuleHandleExA(DWORD, LPCSTR, HMODULE*);
#endif

#undef setprogdir

static void setprogdir(lua_State *L)
{
  char buff[MAX_PATH + 1];
  char *lb;
  DWORD nsize = sizeof(buff);
  DWORD n = GetModuleFileNameA(NULL, buff, nsize);
  if (n == 0 || n == nsize || (lb = strrchr(buff, '\\')) == NULL) {
    luaL_error(L, "unable to get ModuleFileName");
  } else {
    *lb = '\0';
    luaL_gsub(L, lua_tostring(L, -1), LUA_EXECDIR, buff);
    lua_remove(L, -2);  /* remove original string */
  }
}

static void pusherror(lua_State *L)
{
  DWORD error = GetLastError();
#if LJ_TARGET_XBOXONE
  wchar_t wbuffer[128];
  char buffer[128*2];
  if (FormatMessageW(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, error, 0, wbuffer, sizeof(wbuffer)/sizeof(wchar_t), NULL) &&
      WideCharToMultiByte(CP_ACP, 0, wbuffer, 128, buffer, 128*2, NULL, NULL))
#else
  char buffer[128];
  if (FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, error, 0, buffer, sizeof(buffer), NULL))
#endif
    lua_pushstring(L, buffer);
  else
    lua_pushfstring(L, "system error %d\n", error);
}

static void ll_unloadlib(void *lib)
{
  FreeLibrary((HINSTANCE)lib);
}

static void *ll_load(lua_State *L, const char *path, int gl)
{
  HINSTANCE lib = LoadLibraryExA(path, NULL, 0);
  if (lib == NULL) pusherror(L);
  UNUSED(gl);
  return lib;
}

static lua_CFunction ll_sym(lua_State *L, void *lib, const char *sym)
{
  lua_CFunction f = (lua_CFunction)GetProcAddress((HINSTANCE)lib, sym);
  if (f == NULL) pusherror(L);
  return f;
}

static const char *ll_bcsym(void *lib, const char *sym)
{
  if (lib) {
    return (const char *)GetProcAddress((HINSTANCE)lib, sym);
  } else {
    HINSTANCE h = GetModuleHandleA(NULL);
    const char *p = (const char *)GetProcAddress(h, sym);
    if (p == NULL && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(const char *)ll_bcsym, &h))
      p = (const char *)GetProcAddress(h, sym);
    return p;
  }
}

#else

#undef PACKAGE_LIB_FAIL
#define PACKAGE_LIB_FAIL	"absent"

#define DLMSG	"dynamic libraries not enabled; no support for target OS"

static void ll_unloadlib(void *lib)
{
  UNUSED(lib);
}

static void *ll_load(lua_State *L, const char *path, int gl)
{
  UNUSED(path); UNUSED(gl);
  lua_pushliteral(L, DLMSG);
  return NULL;
}

static lua_CFunction ll_sym(lua_State *L, void *lib, const char *sym)
{
  UNUSED(lib); UNUSED(sym);
  lua_pushliteral(L, DLMSG);
  return NULL;
}

static const char *ll_bcsym(void *lib, const char *sym)
{
  UNUSED(lib); UNUSED(sym);
  return NULL;
}

#endif

/* ------------------------------------------------------------------------ */

static void *ll_checkclib (lua_State *L, const char *path) {
  void *plib;
  lua_getfield(L, LUA_REGISTRYINDEX, CLIBS);
  lua_getfield(L, -1, path);
  plib = lua_touserdata(L, -1);  /* plib = CLIBS[path] */
  lua_pop(L, 2);  /* pop CLIBS table and 'plib' */
  return plib;
}


static void ll_addtoclib (lua_State *L, const char *path, void *plib) {
  lua_getfield(L, LUA_REGISTRYINDEX, CLIBS);
  lua_pushlightuserdata(L, plib);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, path);  /* CLIBS[path] = plib */
  lua_rawseti(L, -2, luaL_len(L, -2) + 1);  /* CLIBS[#CLIBS + 1] = plib */
  lua_pop(L, 1);  /* pop CLIBS table */
}


static const char *mksymname(lua_State *L, const char *modname,
			     const char *prefix)
{
  const char *funcname;
  const char *mark = strchr(modname, *LUA_IGMARK);
  if (mark) modname = mark + 1;
  funcname = luaL_gsub(L, modname, ".", "_");
  funcname = lua_pushfstring(L, prefix, funcname);
  lua_remove(L, -2);  /* remove 'gsub' result */
  return funcname;
}

static int ll_loadfunc(lua_State *L, const char *path, const char *name, int r)
{
  void *reg = ll_checkclib(L, path);  /* check loaded C libraries */
  if (reg == NULL) {  /* must load library? */
    reg = ll_load(L, path, *name == '*');
    if (reg == NULL) return PACKAGE_ERR_LIB;
    ll_addtoclib(L, path, reg);
  }
  if (*name == '*') {  /* loading only library (no function)? */
    lua_pushboolean(L, 1);  /* return 'true' */
    return 0;  /* no errors */
  }
  else {
    const char *sym = r ? name : mksymname(L, name, SYMPREFIX_CF);
    lua_CFunction f = ll_sym(L, reg, sym);
    if (f) {
      lua_pushcfunction(L, f);
      return 0;
    }
    if (!r) {
      const char *bcdata = ll_bcsym(reg, mksymname(L, name, SYMPREFIX_BC));
      lua_pop(L, 1);
      if (bcdata) {
	if (luaL_loadbuffer(L, bcdata, LJ_MAX_BUF, name) != 0)
	  return PACKAGE_ERR_LOAD;
	return 0;
      }
    }
    return PACKAGE_ERR_FUNC;  /* Unable to find function. */
  }
}

static int lj_cf_package_loadlib(lua_State *L)
{
  const char *path = luaL_checkstring(L, 1);
  const char *init = luaL_checkstring(L, 2);
  int st = ll_loadfunc(L, path, init, 1);
  if (st == 0) {  /* no errors? */
    return 1;  /* return the loaded function */
  } else {  /* error; error message is on stack top */
    lua_pushnil(L);
    lua_insert(L, -2);
    lua_pushstring(L, (st == PACKAGE_ERR_LIB) ?  PACKAGE_LIB_FAIL : "init");
    return 3;  /* return nil, error message, and where */
  }
}

/* ------------------------------------------------------------------------ */

static int readable(const char *filename)
{
  FILE *f = fopen(filename, "r");  /* try to open file */
  if (f == NULL) return 0;  /* open failed */
  fclose(f);
  return 1;
}

static const char *pushnexttemplate(lua_State *L, const char *path)
{
  const char *l;
  while (*path == *LUA_PATHSEP) path++;  /* skip separators */
  if (*path == '\0') return NULL;  /* no more templates */
  l = strchr(path, *LUA_PATHSEP);  /* find next separator */
  if (l == NULL) l = path + strlen(path);
  lua_pushlstring(L, path, (size_t)(l - path));  /* template */
  return l;
}

static const char *searchpath (lua_State *L, const char *name,
			       const char *path, const char *sep,
			       const char *dirsep)
{
  luaL_Buffer msg;  /* to build error message */
  luaL_buffinit(L, &msg);
  if (*sep != '\0')  /* non-empty separator? */
    name = luaL_gsub(L, name, sep, dirsep);  /* replace it by 'dirsep' */
  while ((path = pushnexttemplate(L, path)) != NULL) {
    const char *filename = luaL_gsub(L, lua_tostring(L, -1),
				     LUA_PATH_MARK, name);
    lua_remove(L, -2);  /* remove path template */
    if (readable(filename))  /* does file exist and is readable? */
      return filename;  /* return that file name */
    lua_pushfstring(L, "\n\tno file " LUA_QS, filename);
    lua_remove(L, -2);  /* remove file name */
    luaL_addvalue(&msg);  /* concatenate error msg. entry */
  }
  luaL_pushresult(&msg);  /* create error message */
  return NULL;  /* not found */
}

static int lj_cf_package_searchpath(lua_State *L)
{
  const char *f = searchpath(L, luaL_checkstring(L, 1),
				luaL_checkstring(L, 2),
				luaL_optstring(L, 3, "."),
				luaL_optstring(L, 4, LUA_DIRSEP));
  if (f != NULL) {
    return 1;
  } else {  /* error message is on top of the stack */
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;  /* return nil + error message */
  }
}

static const char *findfile(lua_State *L, const char *name,
			    const char *pname, const char *dirsep)
{
  const char *path;
  lua_getfield(L, lua_upvalueindex(1), pname);  /* will be at index 3 */
  path = lua_tostring(L, -1);
  if (path == NULL)
    luaL_error(L, LUA_QL("package.%s") " must be a string", pname);
  return searchpath(L, name, path, ".", dirsep);
}

static int checkload (lua_State *L, int stat, const char *filename) {
  if (stat) {  /* module loaded successfully? */
    lua_pushstring(L, filename);  /* will be 2nd argument to module */
    return 2;  /* return open function and file name */
  }
  else
    return luaL_error(L, "error loading module " LUA_QS
                         " from file " LUA_QS ":\n\t%s",
                          lua_tostring(L, 1), filename, lua_tostring(L, -1));
}

static int lj_cf_package_loader_lua(lua_State *L)
{
  const char *filename;
  const char *name = luaL_checkstring(L, 1);
  filename = findfile(L, name, "path", LUA_LSUBSEP);
  if (filename == NULL) return 1;  /* module not found in this path */
  return checkload(L, (luaL_loadfile(L, filename) == LUA_OK), filename);
}

static int lj_cf_package_loader_c(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  const char *filename = findfile(L, name, "cpath", LUA_CSUBSEP);
  if (filename == NULL) return 1;  /* library not found in this path */
  return checkload(L, (ll_loadfunc(L, filename, name, 0) == 0), filename);
}

static int lj_cf_package_loader_croot(lua_State *L)
{
  const char *filename;
  const char *name = luaL_checkstring(L, 1);
  const char *p = strchr(name, '.');
  int st;
  if (p == NULL) return 0;  /* is root */
  lua_pushlstring(L, name, (size_t)(p - name));
  filename = findfile(L, lua_tostring(L, -1), "cpath", LUA_CSUBSEP);
  if (filename == NULL) return 1;  /* root not found */
  if ((st = ll_loadfunc(L, filename, name, 0)) != 0) {
    if (st != PACKAGE_ERR_FUNC)
      return checkload(L, 0, filename);  /* real error */
    lua_pushfstring(L, "\n\tno module " LUA_QS " in file " LUA_QS,
		    name, filename);
    return 1;  /* function not found */
  }
  lua_pushstring(L, filename);  /* will be 2nd argument to module */
  return 2;
}

static int lj_cf_package_loader_preload(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "_PRELOAD");
  lua_getfield(L, -1, name);
  if (lua_isnil(L, -1)) {  /* Not found? */
    const char *bcname = mksymname(L, name, SYMPREFIX_BC);
    const char *bcdata = ll_bcsym(NULL, bcname);
    if (bcdata == NULL || luaL_loadbuffer(L, bcdata, LJ_MAX_BUF, name) != 0)
      lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
  }
  return 1;
}

/* ------------------------------------------------------------------------ */
static void findloader (lua_State *L, const char *name) {
  int i;
  luaL_Buffer msg;  /* to build error message */
  luaL_buffinit(L, &msg);
  lua_getfield(L, lua_upvalueindex(1), "searchers");  /* will be at index 3 */
  if (!lua_istable(L, 3))
    luaL_error(L, LUA_QL("package.searchers") " must be a table");
  /*  iterate over available searchers to find a loader */
  for (i = 1; ; i++) {
    lua_rawgeti(L, 3, i);  /* get a searcher */
    if (lua_isnil(L, -1)) {  /* no more searchers? */
      lua_pop(L, 1);  /* remove nil */
      luaL_pushresult(&msg);  /* create error message */
      luaL_error(L, "module " LUA_QS " not found:%s",
                    name, lua_tostring(L, -1));
    }
    lua_pushstring(L, name);
    lua_call(L, 1, 2);  /* call it */
    if (lua_isfunction(L, -2))  /* did it find a loader? */
      return;  /* module loader found */
    else if (lua_isstring(L, -2)) {  /* searcher returned error message? */
      lua_pop(L, 1);  /* remove extra return */
      luaL_addvalue(&msg);  /* concatenate error message */
    }
    else
      lua_pop(L, 2);  /* remove both returns */
  }
}



static int lj_cf_package_require(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  lua_settop(L, 1);  /* _LOADED table will be at index 2 */
  lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, 2, name);
  if (lua_toboolean(L, -1))  /* is it there? */
    return 1;  /* package is already loaded */
  /* else must load package */
  lua_pop(L, 1);  /* remove 'getfield' result */
  findloader(L, name);
  lua_pushstring(L, name);  /* pass name as argument to module loader */
  lua_insert(L, -2);  /* name is 1st argument (before search data) */
  lua_call(L, 2, 1);  /* run loader to load module */
  if (!lua_isnil(L, -1))  /* non-nil return? */
    lua_setfield(L, 2, name);  /* _LOADED[name] = returned value */
  lua_getfield(L, 2, name);
  if (lua_isnil(L, -1)) {   /* module did not set a value? */
    lua_pushboolean(L, 1);  /* use true as result */
    lua_pushvalue(L, -1);  /* extra copy to be returned */
    lua_setfield(L, 2, name);  /* _LOADED[name] = true */
  }
  lj_lib_checkfpu(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

static void setfenv(lua_State *L)
{
  lua_Debug ar;
  if (lua_getstack(L, 1, &ar) == 0 ||
      lua_getinfo(L, "f", &ar) == 0 ||  /* get calling function */
      lua_iscfunction(L, -1))
    luaL_error(L, LUA_QL("module") " not called from a Lua function");
  lua_pushvalue(L, -2);
  lua_setfenv(L, -2);
  lua_pop(L, 1);
}

static void dooptions(lua_State *L, int n)
{
  int i;
  for (i = 2; i <= n; i++) {
    if (lua_isfunction(L, i)) {  /* avoid 'calling' extra info. */
      lua_pushvalue(L, i);  /* get option (a function) */
      lua_pushvalue(L, -2);  /* module */
      lua_call(L, 1, 0);
    }
  }
}

static void modinit(lua_State *L, const char *modname)
{
  const char *dot;
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "_M");  /* module._M = module */
  lua_pushstring(L, modname);
  lua_setfield(L, -2, "_NAME");
  dot = strrchr(modname, '.');  /* look for last dot in module name */
  if (dot == NULL) dot = modname; else dot++;
  /* set _PACKAGE as package name (full module name minus last part) */
  lua_pushlstring(L, modname, (size_t)(dot - modname));
  lua_setfield(L, -2, "_PACKAGE");
}

static int lj_cf_package_module(lua_State *L)
{
  const char *modname = luaL_checkstring(L, 1);
  int lastarg = lua_gettop(L);  /* index of _LOADED table */
  luaL_pushmodule(L, modname, 1);  /* get/create module table */
  /* check whether table already has a _NAME field */
  lua_getfield(L, -1, "_NAME");
  if (!lua_isnil(L, -1)) {  /* is table an initialized module? */
    lua_pop(L, 1);
  } else {  /* no; initialize it */
    lua_pop(L, 1);
    modinit(L, modname);
  }
  lua_pushvalue(L, -1);
  setfenv(L);
  dooptions(L, lastarg);
  return 0;
}

static int lj_cf_package_seeall(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  if (!lua_getmetatable(L, 1)) {
    lua_createtable(L, 0, 1); /* create new metatable */
    lua_pushvalue(L, -1);
    lua_setmetatable(L, 1);
  }
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_setfield(L, -2, "__index");  /* mt.__index = _G */
  return 0;
}

/*
** __gc tag method for CLIBS table: calls 'll_unloadlib' for all lib
** handles in list CLIBS
*/
static int gctm (lua_State *L) {
  int n = luaL_len(L, 1);
  for (; n >= 1; n--) {  /* for each handle, in reverse order */
    lua_rawgeti(L, 1, n);  /* get handle CLIBS[n] */
    ll_unloadlib(lua_touserdata(L, -1));
    lua_pop(L, 1);  /* pop handle */
  }
  return 0;
}


/* ------------------------------------------------------------------------ */

#define AUXMARK		"\1"

static void setpath(lua_State *L, const char *fieldname, const char *envname1,
                    const char *envname2,
		    const char *def, int noenv)
{
  const char *path =
#if LJ_TARGET_CONSOLE
  NULL;
  UNUSED(envname1); UNUSED(envname2);
#else
  getenv(envname1);
  if (path == NULL)  /* no environment variable? */
    path = getenv(envname2);  /* try alternative name */
#endif
  if (path == NULL || noenv)  /* no environment variable? */
    lua_pushstring(L, def);  /* use default */
  else {
    /* replace ";;" by ";AUXMARK;" and then AUXMARK by default path */
    path = luaL_gsub(L, path, LUA_PATHSEP LUA_PATHSEP,
                              LUA_PATHSEP AUXMARK LUA_PATHSEP);
    luaL_gsub(L, path, AUXMARK, def);
    lua_remove(L, -2);
  }
  setprogdir(L);
  lua_setfield(L, -2, fieldname);
}

static const luaL_Reg package_lib[] = {
  { "loadlib",	lj_cf_package_loadlib },
  { "searchpath",  lj_cf_package_searchpath },
  { "seeall",	lj_cf_package_seeall },
  { NULL, NULL }
};

static const luaL_Reg package_global[] = {
  { "module",	lj_cf_package_module },
  { "require",	lj_cf_package_require },
  { NULL, NULL }
};

static void createsearcherstable (lua_State *L) {
  static const lua_CFunction searchers[] =
  {
    lj_cf_package_loader_preload,
    lj_cf_package_loader_lua,
    lj_cf_package_loader_c,
    lj_cf_package_loader_croot,
    NULL
  };
  int i;
  /* create 'searchers' table */
  lua_createtable(L, sizeof(searchers)/sizeof(searchers[0]) - 1, 0);
  /* fill it with pre-defined searchers */
  for (i=0; searchers[i] != NULL; i++) {
    lua_pushvalue(L, -2);  /* set 'package' as upvalue for all searchers */
    lua_pushcclosure(L, searchers[i], 1);
    lua_rawseti(L, -2, i+1);
  }
}

LUALIB_API int luaopen_package(lua_State *L)
{
  int noenv;
  /* create table CLIBS to keep track of loaded C libraries */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, CLIBS);
  lua_createtable(L, 0, 1);  /* metatable for CLIBS */
  lua_pushcfunction(L, gctm);
  lua_setfield(L, -2, "__gc");  /* set finalizer for CLIBS table */
  lua_setmetatable(L, -2);
  /* create `package' table */
  luaL_newlib(L, package_lib);
  createsearcherstable(L);
  lua_pushvalue(L, -1);  /* make a copy of 'searchers' table */
  lua_setfield(L, -3, "loaders");  /* put it in field `loaders' */
  lua_setfield(L, -2, "searchers");  /* put it in field 'searchers' */
  /* check for noenv */
  lua_getfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  noenv = lua_toboolean(L, -1);
  lua_pop(L, 1);
  /* set field 'path' */
  setpath(L, "path", LUA_PATHVERSION, LUA_PATH, LUA_PATH_DEFAULT, noenv);
  /* set field 'cpath' */
  setpath(L, "cpath", LUA_CPATHVERSION, LUA_CPATH, LUA_CPATH_DEFAULT, noenv);
  /* store config information */
  lua_pushliteral(L, LUA_PATH_CONFIG);
  lua_setfield(L, -2, "config");
  /* set field `loaded' */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_setfield(L, -2, "loaded");
  /* set field `preload' */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "_PRELOAD");
  lua_setfield(L, -2, "preload");
  lua_pushglobaltable(L);
  lua_pushvalue(L, -2);  /* set 'package' as upvalue for next lib */
  luaL_setfuncs(L, package_global, 1);  /* open lib into global table */
  lua_pop(L, 1);  /* pop global table */
  return 1;  /* return 'package' table */
}

