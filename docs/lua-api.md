# Lua API (`engine.*`)

Движок встраивает Lua 5.4 (вендор, `third_party/lua`). Скрипты — data-driven
надстройка над консолью: они видят те же cvar'ы и команды, что игрок, плюс
немного прямых вызовов движка. Реализация — `src/lua_engine.c`, поверхность —
таблица `engine_lib` (`lua_engine.c:863-903`).

## Порядок подключения

1. `boot_init` вызывает `lua_engine_init()` **после** консоли, cvar'ов, биндов
   и FX-пресетов (`main.c:211-223`).
2. Грузится `lua/main.lua` — движковый скрипт (через ресурсную систему, может
   лежать в паке карты). Если файла нет, логируется
   `lua: нет lua/main.lua — скрипты выключены`, и дальнейшие вызовы API не
   происходят.
3. После загрузки мира (в конце `game_load_world`) движок грузит
   `lua/maps/<имя_карты>.lua` (`lua_engine_map`, `lua_engine.c:818-844`).
   Отсутствие файла — не ошибка. Скрипт уже видит заспавненные сущности.
4. При выгрузке карты `lua_engine_clear()` убирает **только** состояние,
   добавленное скриптом карты: update-колбэки, таймеры и подписки на события,
   начиная с индексов, снятых на шаге 3. Движковый `main.lua` остаётся.

Ошибки компиляции и рантайма не валят игру: они печатаются в консоль
(`console_print`), а `pcall` снимает сообщение со стека.

## Песочница

Доступны только `base` (без `dofile`/`loadfile`/`load`, они обнулены —
`lua_engine.c:920-922`), `string`, `table`, `math`. `os`, `io`, `package`,
`debug` не открываются: скрипт карты не может читать файлы и запускать
процессы. Обращение к движку — только через `engine.*`.

## Ограничения

| Что | Лимит |
|---|---|
| Команд из Lua (`engine.cmd`) | 16 (`LUA_CMD_MAX`) |
| Lua-классов сущностей | 16 (`LUACLS_MAX`) |
| Подписок на одно событие | 4 (`LUA_EV_MAX`) |
| Таймеров `engine.after` | 32 (`LUA_TIMERS_MAX`) |
| Update-колбэков | 8 (`LUA_UPDATE_MAX`) |
| `engine.material_register` | до `MATERIALS_MAX` в реестре `material.c` |

Повторная регистрация команды или класса (перезагрузка карты) переиспользует
слот по имени — утечки рефов нет.

---

## Консоль и переменные

| Вызов | Возврат | Примечание |
|---|---|---|
| `engine.log(msg)` | — | Печать в консоль движка |
| `engine.exec(line)` | — | `console_exec`: та же семантика `;` и `wait`, что в консоли |
| `engine.cmd(name, fn)` | — | Новая консольная команда; `fn(args)` получает строку аргументов целиком |
| `engine.cvar_get(name)` | number | 0 для несуществующей переменной |
| `engine.cvar_set(name, v)` | — | С зажимом по диапазону; читы (`CVAR_CHEAT`) не меняются при `sv_cheats 0` |
| `engine.cvar_get_string(name)` | string | Для `CVAR_STRING` (например `name`) |
| `engine.cvar_set_string(name, s)` | — | |
| `engine.bind(key, cmd)` / `engine.unbind(key)` | — | Имя клавиши — SDL scancode-имя или `mouse_wheel_up`/`mouse_wheel_down`; неизвестная клавиша → сообщение в консоль |
| `engine.msg(text)` | — | Сообщение карты по центру экрана, 4 секунды (как у триггеров) |
| `engine.time()` | number | Секунды с запуска движка |

## События и цикл

| Вызов | Что |
|---|---|
| `engine.on("событие", fn)` | Подписка; `fn(type, a, b)` получает код события и два `lightuserdata`-параметра |
| `engine.update(fn)` | Вызов каждый кадр: `fn(dt)` |
| `engine.after(sec, fn)` | Таймер на один выстрел |

События: `entity_spawn`, `entity_kill`, `console_open`, `console_close`
(`ev_type_from_name`, `lua_engine.c:139-146`). unknown имя →
`engine.on: неизвестное событие`.

Колбэки, добавленные во время `update`/таймера, в этом тике не вызываются —
съедут на следующий кадр (`lua_engine.c:748-811`).

## Сущности

Сущность передаётся в Lua **хэндлом** (`int`), а не указателем: мёртвая
сущность даёт `nil`/пустой ответ, повисшей ссылки нет.

| Вызов | Возврат |
|---|---|
| `engine.entity_class(name, { spawn=fn, think=fn, kill=fn })` | — ; колбэки вызываются как `fn(handle, dt)` |
| `engine.entity_spawn(class, [x, y, z])` | handle или `nil` |
| `engine.entity_kill(h)` | — |
| `engine.entity_find(class, [n])` | handle n-й сущности класса (0 — первая) или `nil` |
| `engine.entity_count()` | число |
| `engine.entity_pos(h)` | `x, y, z` (нули для мёртвой) |
| `engine.entity_set_pos(h, x, y, z)` | — |
| `engine.entity_classname(h)` | строка (пустая для мёртвой) |
| `engine.world_loaded()` | bool |

## Мир, камера, игрок

| Вызов | Возврат / действие |
|---|---|
| `engine.camera()` | `x, y, z, yaw, pitch` (взгляд игрока; нули без игры) |
| `engine.terrain_height(x, z)` | высота рельефа, 0 до загрузки мира |
| `engine.teleport(x, y, z, [yaw], [pitch])` | телепорт игрока (углы в радианах) |
| `engine.player_hp()` | `hp, max, dead` |
| `engine.player_damage(n)` | урон игроку |
| `engine.player_heal([n])` | лечение; без n — до полного |
| `engine.player_set_max(n)` | новый максимум (долить до полного) |

## Модели, материалы, эффекты

| Вызов | Возврат / действие |
|---|---|
| `engine.mesh(path, [x, y, z], [scale], [tex])` | bool — статичный `.obj` узлом сцены |
| `engine.glb(path, [x, y, z], [scale])` | bool — `.glb`; при наличии анимации узел будет скининговым (`AnimInstance3D`) |
| `engine.material_register(name, { … })` | true; поля таблицы: `tint={r,g,b}`, `alpha`, `blend` (0/1/2/3), `tile={u,v}` |
| `engine.material_list()` | строка имён через пробел (для отладки) |
| `engine.fx_burst(name, [x, y, z], [count])` | всплеск частиц |
| `engine.fx_emit(name, [x, y, z], [rate], [life])` | эмиттер (rate по умолчанию 10) |
| `engine.fx_stop([name])` | остановить эмиттеры |
| `engine.fx_clear()` | все эмиттеры + пул |
| `engine.fx_weather([name], [rate])` | погода за игроком; `""`/`"off"`/`"clear"` — стоп. Area фиксирована: ext 20×10×20, высота 12 м |
| `engine.fx_register(name, { … })` | свой пресет частиц (см. ниже) |

Поля `engine.fx_register` (`lua_engine.c:644-711`) и их дефолты:

| Поле | Дефолт | Смысл |
|---|---|---|
| `dir` | `{0, 1, 0}` | Направление выброса |
| `spread` | 0.5 | Разброс, радианы |
| `speed` | `{1, 2}` | min/max, м/с |
| `life` | `{0.5, 1.0}` | min/max, секунды |
| `size` | `{0.2, 0.2}` | min/max, метры |
| `alpha` | `{1.0, 0.0}` | от/до |
| `color` | `{1, 1, 1}` | RGB |
| `gravity` | 0 | м/с² |
| `drag` | 0 | Сопротивление |
| `sway` | 0 | Покачивание |
| `wind` | `{0, 0, 0}` | Ветер |
| `blend` | 0 | 1 = аддитивное смешение |
| `streak` | 0 | 1 = вытянутые по скорости |
| `plain` | 0 | 1 = без освещения/текстуры |
| `bounce` | 0 | 1 = отскок от рельефа |
| `collide` | 0 | 1 = гибнет о террейн/воду, 2 = ложится и гаснет (снег) |
| `count` | 8 | Частиц на всплеск |

## Пример (`lua/main.lua`, сокращённо)

```lua
engine.cmd("lua_greet", function(args)
    engine.log("[Lua] привет, " .. ((args ~= "" and args) or "мир") .. "!")
end)

engine.on("console_open",  function() engine.log("[Lua] консоль открыта")  end)
engine.on("console_close", function() engine.log("[Lua] консоль закрыта") end)

engine.entity_class("beacon", {
    spawn = function(e) engine.log("[Lua] маяк: " .. engine.entity_classname(e)) end,
    think = function(e, dt)
        local x, y, z = engine.entity_pos(e)
        engine.entity_set_pos(e, x, 5 + math.sin(engine.time()) * 1.5, z)
    end,
    kill  = function() engine.log("[Lua] маяк исчез") end,
})

engine.update(function(dt)
    if not world_up and engine.world_loaded() then
        world_up = true
        engine.entity_spawn("beacon", 1000, 5, 562)
    end
end)
```

## См. также

- [commands.md](commands.md) — что вызывает `engine.exec`
- [cvars.md](cvars.md) — что крутят `engine.cvar_set`
- [map-json.md](map-json.md) — секции `cvars`/`commands` внутри карты
