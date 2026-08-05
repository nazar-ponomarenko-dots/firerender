-- firerender: движковый скрипт (Lua 5.4)
-- Таблица engine: log / cvar_get / cvar_set / exec / cmd / on / update
engine.log("[Lua] скрипты загружены (firerender)")

-- Консольная команда из Lua
engine.cmd("lua_greet", function(args)
    local a = args or ""
    if a == "" then a = "мир" end
    engine.log("[Lua] привет, " .. a .. "!")
end)

-- Событие: консоль открыта/закрыта
engine.on("console_open", function()
    engine.log("[Lua] консоль открыта")
end)
engine.on("console_close", function()
    engine.log("[Lua] консоль закрыта")
end)

-- Per-frame: раз в 2 секунды показываем состояние движка
local frames = 0
engine.update(function(dt)
    frames = frames + 1
    if frames % 120 == 0 then
        engine.log(string.format("[Lua] кадров %d | fov=%.0f | shadows=%s",
            frames, engine.cvar_get("fov"),
            engine.cvar_get("r_shadows_mode") > 0.5 and "ON" or "OFF"))
    end
end)
