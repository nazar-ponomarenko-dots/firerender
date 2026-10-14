-- ============================================================================
--  Map script for map1 (lua/maps/map1.lua)
--  Runs AFTER the world is loaded: entities are already spawned, you can
--  find them, move them, spawn new ones, set timers and show text.
-- ============================================================================
engine.log("[map1] скрипт карты")

-- Script-controlled beacon: hovers above the field
engine.entity_class("map1_beacon", {
    spawn = function(e)
        engine.log("[map1] beacon спавн")
    end,
    think = function(e, dt)
        local x, y, z = engine.entity_pos(e)
        engine._t = (engine._t or 0) + dt
        engine.entity_set_pos(e, x, 12 + math.sin(engine._t) * 2, z)
    end,
})

-- 2 s after load: spawn the beacon near the player + terrain height
engine.after(2.0, function()
    local x, y, z = engine.camera()
    engine.entity_spawn("map1_beacon", x + 5, 12, z)
    engine.log("[map1] высота под игроком: " .. string.format("%.1f", engine.terrain_height(x, z)))
end)

-- After 8 s: on-screen greeting
engine.after(8.0, function()
    engine.msg("Добро пожаловать в map1! (из Lua)")
end)
