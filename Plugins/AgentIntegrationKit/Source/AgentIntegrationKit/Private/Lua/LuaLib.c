// Compiles the entire Lua 5.4.7 library as a single C translation unit.
// UBT will compile this .c file with the C compiler, giving us C linkage
// that sol2 expects (SOL_USING_CXX_LUA=0).

// Export Lua C API symbols so extension modules (AIK_PoseSearch, etc.) can link them.
// On Windows: dllexport. On Mac/Linux: visibility("default") to override -fvisibility=hidden.
// LUA_CORE is defined by the Lua sources themselves; we only need LUA_BUILD_AS_DLL here.
#define LUA_BUILD_AS_DLL

#include "LuaAll.c"
