// ============================================================================
//  commands.h — console action commands (see commands.c)
// ----------------------------------------------------------------------------
//  Command handlers that used to live in main.c: movement toggles,
//  maps, save/load/setpos, entities, screenshot, sound. Command REGISTRATION
//  (console_add_command) stayed in main.c — the whole list is visible there at once.
// ============================================================================
#ifndef COMMANDS_H
#define COMMANDS_H

// --- Action commands (bound to keys in config.cfg) ------------------------
void cmd_noclip(const char *args);
void cmd_toggle_map(const char *args);
void cmd_toggle_debug(const char *args);
void cmd_toggle_crosshair(const char *args);
void cmd_toggle_shadows(const char *args);
void cmd_toggle_fog(const char *args);
void cmd_demo(const char *args);
void cmd_pause(const char *args);

// Map selection: map <name> — a "map" (folder) or a name from maps/*.zip.
void cmd_map_select(const char *args);
void cmd_maps_list(const char *args);

// HL-style: commands adapted to our systems
void cmd_restart(const char *args);
void cmd_changelevel(const char *args);

void cmd_r_drawdebug(const char *args);
void cmd_mat_wireframe(const char *args);
void cmd_thirdperson(const char *args);
void cmd_firstperson(const char *args);

// Entities (read: debug foundation for Lua)
void cmd_ent_create(const char *args);
void cmd_ent_list(const char *args);

// Screenshot / sound
void cmd_screenshot(const char *args);
void cmd_snd_test(const char *args);

// Applies the saved world state (triggered triggers, use-objects)
// after loading a map. Auto-called from game_load_world and cmd_load.
void commands_apply_saved_world_state(void);

// Save/Load (F5/F9): player position + map in save.cfg
void cmd_save(const char *args);
void cmd_load(const char *args);

// Player teleport: setpos x y z [yaw] [pitch]
void cmd_setpos(const char *args);
void cmd_mesh(const char *args);
// Place a binary glTF (.glb): glb <path> [x y z] [scale]
void cmd_glb(const char *args);
// Place an animated skinning glTF (.glb): glbanim <path> [x y z] [scale]
void cmd_glbanim(const char *args);
// Mini-physics (Verlet): rope in front of the camera, push, clear
void cmd_phys_rope(const char *args);
void cmd_phys_push(const char *args);
void cmd_phys_clear(const char *args);
// Ragdoll on the last animated model: phys_ragdoll [hang] [kick]
void cmd_phys_ragdoll(const char *args);
// List animation clips of an anim node: glbclips [node-name]
void cmd_glbclips(const char *args);
// Switch clip: glbclip <index|name> [node-name]
void cmd_glbclip(const char *args);
// Playback control
void cmd_glbanim_pause(const char *args);   // pause
void cmd_glbanim_play(const char *args);    // resume
void cmd_glbanim_speed(const char *args);   // glbanim_speed <x>
void cmd_glbanim_loop(const char *args);    // glbanim_loop <0|1>
void cmd_glbanim_seek(const char *args);    // glbanim_seek <seconds> [node-name]
void cmd_glbanim_frame(const char *args);   // glbanim_frame <frame> [fps] [node-name]
void cmd_anim(const char *args);            // anim tool (registry by index)
// Attaching nodes to models/bones: attach sprite|glb|mesh <N> <bone> <file>
void cmd_attach(const char *args);
// Free sprite in the world: sprite <png> [x y z] [w h] [flat]
void cmd_sprite(const char *args);
void cmd_matlist(const char *args);

// Health (HP): status, damage/heal, invulnerability, death, limits
void cmd_hp(const char *args);
void cmd_hurt(const char *args);
void cmd_heal(const char *args);
void cmd_god(const char *args);
void cmd_kill(const char *args);
void cmd_sethp(const char *args);
void cmd_maxhp(const char *args);

// Effects (fx) — particles at the point where the look ray hits the terrain
void cmd_fx_sparks(const char *args);
void cmd_fx_smoke(const char *args);
void cmd_fx_blood(const char *args);

// Shader hot-reload: recompiles the changed (by mtime) files.
void cmd_reload_shaders(const char *args);

// Cubemap: render 6 faces from the player position (or a point) + 6 PNGs into cubemaps/
void cmd_cubemap_capture(const char *args);

// Called from the main loop every frame: saves the frame to a PNG if the
// screenshot command (F12) was requested.
void commands_shot_take(int vw, int vh);

#endif
