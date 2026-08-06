-- ============================================================================
--  firerender: движковый скрипт (Lua 5.4)
--  Таблица `engine` (см. src/lua_engine.c):
--    log / cvar_get / cvar_set / exec / cmd / on / update
--    entity_class / entity_spawn / entity_kill / entity_find / entity_count
--    entity_pos / entity_set_pos / entity_classname / world_loaded
--  События (engine.on): entity_spawn, entity_kill, console_open, console_close.
-- ============================================================================

engine.log("[Lua] скрипты загружены (firerender)")

-- --- Консольная команда из Lua -------------------------------------------------
engine.cmd("lua_greet", function(args)
    local who = (args ~= "" and args) or "мир"
    engine.log("[Lua] привет, " .. who .. "!")
end)

-- --- События консоли -------------------------------------------------------------
engine.on("console_open",  function() engine.log("[Lua] консоль открыта")  end)
engine.on("console_close", function() engine.log("[Lua] консоль закрыта")  end)

-- --- Скриптовая сущность: маяк, качается на месте --------------------------------
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

-- --- После загрузки мира: спавн маяка -------------------------------------------
local world_up = false
engine.update(function()
    if not world_up and engine.world_loaded() then
        world_up = true
        engine.entity_spawn("beacon", 950, 5, 540)
        engine.log("[Lua] сущностей после загрузки: " .. engine.entity_count())
    end
end)

-- --- Эффекты частиц (engine.fx_*) ------------------------------------------------
-- Регистрация своего пресета из Lua (параметры — как ParticleParams в particles.h)
engine.fx_register("magic", {
    dir      = { 0, 1, 0 },   -- вверх
    spread   = 0.7,           -- широкий конус
    speed    = { 1.0, 3.0 },  -- м/с
    life     = { 0.5, 1.2 },  -- с
    size     = { 0.3, 0.05 }, -- старт -> финал
    alpha    = { 1.0, 0.0 },  -- fade out
    color    = { 0.4, 1.0, 0.8 },
    gravity  = -0.5,          -- всплывает
    drag     = 1.0,
    sway     = 0.6,           -- покачивание
    blend    = 1,             -- аддитив (светится)
    count    = 20,
})

-- Команда lua_fx: engine.fx_burst("magic", x, y, z)
engine.cmd("lua_fx", function()
    local x, y, z = engine.camera()
    engine.fx_burst("magic", x, y, z + 1, 0)
    engine.log("[Lua] магия: fx_burst(magic)")
end)

-- Команда lua_rain: engine.fx_emit("rain", ...) на 15 секунд
engine.cmd("lua_rain", function()
    local x, y, z = engine.camera()
    engine.fx_emit("rain", x, y + 15, z, 200, 15)
    engine.log("[Lua] дождь: fx_emit(rain) на 15 сек")
end)
