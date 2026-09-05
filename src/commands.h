// ============================================================================
//  commands.h — консольные команды-действия (см. commands.c)
// ----------------------------------------------------------------------------
//  Обработчики команд, которые раньше жили в main.c: движение-тумблеры,
//  карты, save/load/setpos, сущности, скриншот, звук. РЕГИСТРАЦИЯ команд
//  (console_add_command) осталась в main.c — там виден весь список сразу.
// ============================================================================
#ifndef COMMANDS_H
#define COMMANDS_H

// --- Команды-действия (вешаются на бинды в config.cfg) ------------------------
void cmd_noclip(const char *args);
void cmd_toggle_map(const char *args);
void cmd_toggle_debug(const char *args);
void cmd_toggle_crosshair(const char *args);
void cmd_toggle_shadows(const char *args);
void cmd_toggle_fog(const char *args);
void cmd_demo(const char *args);
void cmd_pause(const char *args);

// Выбор карты: map <имя> — "map" (папка) или имя из maps/*.zip.
void cmd_map_select(const char *args);
void cmd_maps_list(const char *args);

// HL-стиль: команды, адаптированные под наши системы
void cmd_restart(const char *args);
void cmd_changelevel(const char *args);

void cmd_r_drawdebug(const char *args);
void cmd_mat_wireframe(const char *args);
void cmd_thirdperson(const char *args);
void cmd_firstperson(const char *args);

// Сущности (читай — дебаг фундамента для Lua)
void cmd_ent_create(const char *args);
void cmd_ent_list(const char *args);

// Скриншот / звук
void cmd_screenshot(const char *args);
void cmd_snd_test(const char *args);

// Применяет сохранённое состояние мира (сработавшие триггеры, use-объекты)
// после загрузки карты. Авто-вызов из game_load_world и cmd_load.
void commands_apply_saved_world_state(void);

// Save/Load (F5/F9): позиция игрока + карта в save.cfg
void cmd_save(const char *args);
void cmd_load(const char *args);

// Телепорт игрока: setpos x y z [yaw] [pitch]
void cmd_setpos(const char *args);
void cmd_mesh(const char *args);
// Поставить бинарный glTF (.glb): glb <путь> [x y z] [scale]
void cmd_glb(const char *args);
// Поставить анимированный glTF (.glb) со скиннингом: glbanim <путь> [x y z] [scale]
void cmd_glbanim(const char *args);
// Мини-физика (Verlet): верёвка перед камерой, толчок, очистка
void cmd_phys_rope(const char *args);
void cmd_phys_push(const char *args);
void cmd_phys_clear(const char *args);
// Список клипов анимации анимик-узла: glbclips [имя-узла]
void cmd_glbclips(const char *args);
// Переключить клип: glbclip <индекс|имя> [имя-узла]
void cmd_glbclip(const char *args);
// Управление воспроизведением
void cmd_glbanim_pause(const char *args);   // пауза
void cmd_glbanim_play(const char *args);    // продолжить
void cmd_glbanim_speed(const char *args);   // glbanim_speed <x>
void cmd_glbanim_loop(const char *args);    // glbanim_loop <0|1>
void cmd_glbanim_seek(const char *args);    // glbanim_seek <сек> [имя-узла]
void cmd_glbanim_frame(const char *args);   // glbanim_frame <кадр> [fps] [имя-узла]
void cmd_anim(const char *args);            // инструмент anim (реестр по индексу)
// Крепление узлов к моделям/костям: attach sprite|glb|mesh <N> <кость> <файл>
void cmd_attach(const char *args);
// Свободный спрайт в мире: sprite <png> [x y z] [w h] [flat]
void cmd_sprite(const char *args);
void cmd_matlist(const char *args);

// Эффекты (fx) — частицы на точке попадения луча взгляда в рельеф
void cmd_fx_sparks(const char *args);
void cmd_fx_smoke(const char *args);
void cmd_fx_blood(const char *args);

// Hot-reload шейдеров: перекомпилирует изменённые (по mtime) файлы.
void cmd_reload_shaders(const char *args);

// Кубомап: рендер 6 граней из позиции игрока (или точки) + 6 PNG в cubemaps/
void cmd_cubemap_capture(const char *args);

// Вызывается из главного цикла каждый кадр: сохраняет кадр в PNG, если
// была запрошена команда screenshot (F12).
void commands_shot_take(int vw, int vh);

#endif
