#include <string>
#include <iostream>
extern "C"
{
#include "./src/lua.h"
#include "./src/lauxlib.h"
#include "./src/lualib.h"

static int add(lua_State* L){
    double d1 = luaL_checknumber(L, 1);
    double d2 = luaL_checknumber(L, 2);
    lua_pushnumber(L, d1 + d2);
    return 1;
}

static int str_find(lua_State* L){
    const char* pText = luaL_checkstring(L, 1);
    const char* pPattern = luaL_checkstring(L, 2);
    std::string strText(pText);
    std::string::size_type pos = strText.find(pPattern);
    if(pos != std::string::npos){
        lua_pushnumber(L, pos);
    }else
    {
        lua_pushnil(L);
    }
    return 1;
}

static int str_concat(lua_State* L){
    const char* pHead = luaL_checkstring(L, 1);
    const char* pTail = luaL_checkstring(L, 2);
    std::cout << "pHead = " << pHead << std::endl;
    std::cout << "pTail = " << pTail << std::endl;
    std::string strRes = std::string(pHead) + std::string(pTail);
    std::cout << "strRes = " << strRes << std::endl;
    lua_pushlstring(L, strRes.c_str(), strRes.size());
    return 1;
}

static const struct luaL_Reg test_lib[] = {
    {"add", add},
    {"str_find", str_find},
    {"str_concat", str_concat},
    {nullptr, nullptr}
};

int luaopen_test_lib(lua_State* L){
    luaL_register(L, "test_lib", test_lib); // lua 5.1
    return 1;
}
}

