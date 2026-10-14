-- ============================================================================
--  firerender: engine script (Lua 5.4)
--  The `engine` table (see src/lua_engine.c):
--    log / cvar_get / cvar_set / exec / cmd / on / update
--    entity_class / entity_spawn / entity_kill / entity_find / entity_count
--    entity_pos / entity_set_pos / entity_classname / world_loaded
--  Events (engine.on): entity_spawn, entity_kill, console_open, console_close.
-- ============================================================================

engine.log("[Lua] скрипты загружены (firerender)")

-- --- Console command defined from Lua -------------------------------------------
engine.cmd("lua_greet", function(args)
    local who = (args ~= "" and args) or "мир"
    engine.log("[Lua] привет, " .. who .. "!")
end)

-- --- Console events ---------------------------------------------------------------
engine.on("console_open",  function() engine.log("[Lua] консоль открыта")  end)
engine.on("console_close", function() engine.log("[Lua] консоль закрыта")  end)

-- --- Scripted entity: a beacon bobbing in place -----------------------------------
engine.entity_class("beacon", {
    spawn = function(e)
        engine.log("[Lua] маяк появился (" .. engine.entity_classname(e) .. ")")
    end,
    think = function(e, dt)
        local x, y, z = engine.entity_pos(e)
        local t = (engine._t or 0) + dt
        engine._t = t
        engine.entity_set_pos(e, x, 5 + math.sin(t) * 1.5, z)
    end,
    kill = function(e)
        engine.log("[Lua] маяк исчез")
    end,
})

-- --- After the world loads: spawn the beacon --------------------------------------
local world_up = false
engine.update(function()
    if not world_up and engine.world_loaded() then
        world_up = true
        engine.entity_spawn("beacon", 950, 5, 540)
        engine.log("[Lua] сущностей после загрузки: " .. engine.entity_count())
    end
end)

-- --- Particle effects (engine.fx_*) ------------------------------------------------
-- Registering a custom preset from Lua (parameters mirror ParticleParams in particles.h)
engine.fx_register("magic", {
    dir      = { 0, 1, 0 },   -- upwards
    spread   = 0.7,           -- wide cone
    speed    = { 1.0, 3.0 },  -- m/s
    life     = { 0.5, 1.2 },  -- s
    size     = { 0.3, 0.05 }, -- start -> end
    alpha    = { 1.0, 0.0 },  -- fade out
    color    = { 0.4, 1.0, 0.8 },
    gravity  = -0.5,          -- floats up
    drag     = 1.0,
    sway     = 0.6,           -- sideways wobble
    blend    = 1,             -- additive (glows)
    count    = 20,
})

-- Command lua_fx: engine.fx_burst("magic", x, y, z)
engine.cmd("lua_fx", function()
    local x, y, z = engine.camera()
    engine.fx_burst("magic", x, y, z + 1, 0)
    engine.log("[Lua] магия: fx_burst(magic)")
end)

-- Command lua_rain: engine.fx_weather("rain") — rain behind the player
engine.cmd("lua_rain", function(args)
    if args == "off" then
        engine.fx_weather("off")
        engine.log("[Lua] дождь выключен")
    else
        engine.fx_weather("rain", 200)
        engine.log("[Lua] дождь: fx_weather(rain)")
    end
end)
