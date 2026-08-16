-- ============================================================================
--  Скрипт карты map1 (lua/maps/map1.lua)
--  Выполняется ПОСЛЕ загрузки мира: сущности уже заспавнены, можно их
--  находить, двигать, спавнить новых, вешать таймеры и показывать текст.
-- ============================================================================
engine.log("[map1] скрипт карты")

-- Управляемый скриптом маяк: плавает над полем
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

-- Через 2с после загрузки: спавн маяка у игрока + высота рельефа
engine.after(2.0, function()
    local x, y, z = engine.camera()
    engine.entity_spawn("map1_beacon", x + 5, 12, z)
    engine.log("[map1] высота под игроком: " .. string.format("%.1f", engine.terrain_height(x, z)))
end)

-- Через 8с: приветствие на экране
engine.after(8.0, function()
    engine.msg("Добро пожаловать в map1! (из Lua)")
end)
