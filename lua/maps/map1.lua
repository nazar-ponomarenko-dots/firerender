-- ============================================================================
--  Скрипт карты map1 (lua/maps/map1.lua)
--  Выполняется ПОСЛЕ загрузки мира: сущности уже заспавнены, можно их
--  находить, двигать, спавнить новых и вешать таймеры.
-- ============================================================================
engine.log("[map1] скрипт карты")

-- Управляемый скриптом маяк: светится и плавает над полем
engine.entity_class("map1_beacon", {
    spawn = function(e)
        engine.log("[map1] beacon спавн")
    end,
    think = function(e, dt)
        local x, y, z = engine.entity_pos(e)
        engine.entity_set_pos(e, x, 12 + math.sin((engine._t or 0)) * 2, z)
        engine._t = (engine._t or 0) + dt
    end,
})

-- Через 2 секунды после загрузки карты — спавн маяка
engine.after(2.0, function()
    local x, y, z = engine.camera()          -- позиция игрока
    engine.entity_spawn("map1_beacon", x + 5, 12, z)
    engine.log("[map1] высота рельефа под игроком: " .. string.format("%.1f", engine.terrain_height(x, z)))
end)
