#include "lua_bridge_internal.h"

void lua_bridge_game_camera_init(lua_State *L);
void lua_bridge_game_scene_init(lua_State *L);
void lua_bridge_game_save_init(lua_State *L);

void lua_bridge_game_init(lua_State *L) {
    // Create the game table
    lua_newtable(L);
    
    // Add camera submodule
    lua_pushstring(L, "camera");
    lua_bridge_game_camera_init(L);
    lua_settable(L, -3);
    
    // Add scene submodule
    lua_pushstring(L, "scene");
    lua_bridge_game_scene_init(L);
    lua_settable(L, -3);
    
    // Add save submodule
    lua_pushstring(L, "save");
    lua_bridge_game_save_init(L);
    lua_settable(L, -3);
    
    // Register as picocalc.game
    lua_setfield(L, -2, "game");
}
