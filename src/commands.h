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
void cmd_toggle_hud(const char *args);
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

void cmd_r_drawhud(const char *args);
void cmd_mat_wireframe(const char *args);
void cmd_thirdperson(const char *args);
void cmd_firstperson(const char *args);

// Сущности (читай — дебаг фундамента для Lua)
void cmd_ent_create(const char *args);
void cmd_ent_list(const char *args);

// Скриншот / звук
void cmd_screenshot(const char *args);
void cmd_snd_test(const char *args);

// Save/Load (F5/F9): позиция игрока + карта в save.cfg
void cmd_save(const char *args);
void cmd_load(const char *args);

// Телепорт игрока: setpos x y z [yaw] [pitch]
void cmd_setpos(const char *args);

// Вызывается из главного цикла каждый кадр: сохраняет кадр в PNG, если
// была запрошена команда screenshot (F12).
void commands_shot_take(int vw, int vh);

#endif
