// ============================================================================
//  cmdreg.c — ЕДИНЫЙ реестр консольных команд (см. cmdreg.h)
// ----------------------------------------------------------------------------
//  Все статические команды движка описаны здесь — в одной таблице с_cmds.
//  cmdreg_register_all() (единственная регистрация) вызывается из boot_init
//  один раз после console_init. Порядок = порядок в таблице.
//
//  Обработчики лежат в своих модулях (commands.c, fx.c), матрица действия
//  (g_act) — в actions.c, но определение команд собрано здесь, чтобы не
//  раскидывать console_add_command по полдюжине файлов.
// ============================================================================
#include "cmdreg.h"

#include "actions.h"     // g_act + ActionId
#include "commands.h"    // команды-действия
#include "material.h"    // (cmd_matlist в commands.c)
#include "fx.h"          // команды эффектов
#include "glb.h"         // cmd_glb
#include "glb_anim.h"    // cmd_glbanim + auto-выбор в cmd_glb

// --- +/- команды действия (ставят флаг g_act) ---------------------------------
#define DEF_ACTION(id, name)                                             \
    static void a_##name##_on(const char *a) { (void)a; g_act[id] = 1; } \
    static void a_##name##_off(const char *a) { (void)a; g_act[id] = 0; }

DEF_ACTION(A_FORWARD, forward)
DEF_ACTION(A_BACK,    back)
DEF_ACTION(A_LEFT,    left)
DEF_ACTION(A_RIGHT,   right)
DEF_ACTION(A_JUMP,    jump)
DEF_ACTION(A_DOWN,    down)
DEF_ACTION(A_SPEED,   speed)
DEF_ACTION(A_DUCK,    duck)
DEF_ACTION(A_USE,     use)

// --- Центральная таблица команд -------------------------------------------------
// Добавить новую команду = добавить одну строку здесь (и реализовать
// обработчик в своём модуле). Никаких console_add_command в инициализациях.
static const ConCmdDesc s_cmds[] = {
    // Движение (+/- команды, читает camera.c)
    { "+forward",  "движение вперёд",        a_forward_on },
    { "-forward",  "",                       a_forward_off },
    { "+back",     "движение назад",         a_back_on },
    { "-back",     "",                       a_back_off },
    { "+left",     "движение влево",         a_left_on },
    { "-left",     "",                       a_left_off },
    { "+right",    "движение вправо",        a_right_on },
    { "-right",    "",                       a_right_off },
    { "+jump",     "прыжок / вверх в полёте", a_jump_on },
    { "-jump",     "",                       a_jump_off },
    { "+down",     "вниз в полёте",          a_down_on },
    { "-down",     "",                       a_down_off },
    { "+speed",    "бег (ускорение)",        a_speed_on },
    { "-speed",    "",                       a_speed_off },
    { "+duck",     "присесть",               a_duck_on },
    { "-duck",     "",                       a_duck_off },
    { "+use",      "взаимодействие (E)",     a_use_on },
    { "-use",      "",                       a_use_off },

    // Эффекты частиц (fx.c)
    { "fx",        "эффект: fx <имя> [x y z]", cmd_fx },
    { "fx_burst",  "fx_burst <имя> <x> <y> <z> [count]", cmd_fx_burst },
    { "fx_emit",   "fx_emit <имя> <x> <y> <z> <rate> [life]", cmd_fx_emit },
    { "fx_stop",   "fx_stop [имя] — остановить эмиттер(ы)", cmd_fx_stop },
    { "fx_clear",  "fx_clear — все эмиттеры + пул", cmd_fx_clear },
    { "fx_list",   "fx_list — список пресетов", cmd_fx_list },
    { "weather",   "weather <пресет> [rate] | off — погода за игроком", cmd_weather },

    // Команды-действия (commands.c) — доступны сразу, в т.ч. в меню
    { "toggle_map",    "показать/скрыть мини-карту", cmd_toggle_map },
    { "toggle_hud",    "показать/скрыть HUD",        cmd_toggle_hud },
    { "toggle_shadows","вкл/выкл тени",              cmd_toggle_shadows },
    { "toggle_fog",    "вкл/выкл объёмный туман",    cmd_toggle_fog },
    { "demo",          "демо-сцена (автополёт, замер FPS)", cmd_demo },
    { "pause",         "пауза / назад в меню",       cmd_pause },
    { "map",           "map <имя> — выбрать и запустить карту", cmd_map_select },
    { "maps",          "список карт",                cmd_maps_list },
    { "noclip",        "полёт сквозь мир (чит)",     cmd_noclip },
    { "restart",       "перезапустить текущую карту", cmd_restart },
    { "changelevel",   "changelevel <карта>",        cmd_changelevel },
    { "r_drawhud",     "r_drawhud [0/1] — HUD",      cmd_r_drawhud },
    { "mat_wireframe", "mat_wireframe [0/1]",        cmd_mat_wireframe },
    { "thirdperson",   "вид от третьего лица",       cmd_thirdperson },
    { "firstperson",   "вид от первого лица",        cmd_firstperson },
    { "ent_create",    "ent_create <класс> [x y z]", cmd_ent_create },
    { "ent_list",      "список сущностей",           cmd_ent_list },
    { "snd_test",      "проиграть все звуки подряд", cmd_snd_test },
    { "screenshot",    "сохранить кадр в PNG (F12)", cmd_screenshot },
    { "save",          "сохранить позицию игрока (F5)", cmd_save },
    { "load",          "загрузить сохранение (F9)",  cmd_load },
    { "setpos",        "setpos x y z [yaw] [pitch] — телепорт", cmd_setpos },
    { "reload_shaders","перекомпилировать изменённые шейдеры", cmd_reload_shaders },
    { "fx_sparks",     "искры в точке взгляда (M)",  cmd_fx_sparks },
    { "fx_smoke",      "дым в точке взгляда (,)",    cmd_fx_smoke },
    { "fx_blood",      "кровь в точке взгляда (.)",  cmd_fx_blood },
    { "matlist",       "список зарегистрированных материалов", cmd_matlist },
    { "mesh",          "mesh <obj> [x y z] [scale] [tex] — статичный меш", cmd_mesh },
    { "glb",           "glb <glb> [x y z] [scale] — меш (.glb), скиннинг если есть", cmd_glb },
    { "glbanim",       "glbanim <glb> [x y z] [scale] — анимир. меш (skinning)", cmd_glbanim },
    { "glbclips",      "glbclips [имя-узла] — список клипов анимации", cmd_glbclips },
    { "glbclip",       "glbclip <индекс|имя> [имя-узла] — переключить клип", cmd_glbclip },
    { "glbanim_pause", "glbanim_pause [имя-узла] — пауза анимации", cmd_glbanim_pause },
    { "glbanim_play",  "glbanim_play [имя-узла] — продолжить анимацию", cmd_glbanim_play },
    { "glbanim_speed", "glbanim_speed <x> [имя-узла] — скорость анимации", cmd_glbanim_speed },
    { "glbanim_loop",  "glbanim_loop <0|1> [имя-узла] — зациклить анимацию", cmd_glbanim_loop },
    { "cubemap_capture", "cubemap_capture [x y z] [size] — захват кубомапа отражений (6 PNG в cubemaps/)", cmd_cubemap_capture },
};

void cmdreg_register_all(void)
{
    for (size_t i = 0; i < sizeof s_cmds / sizeof s_cmds[0]; i++)
        console_add_command(s_cmds[i].name, s_cmds[i].help, s_cmds[i].fn);
}