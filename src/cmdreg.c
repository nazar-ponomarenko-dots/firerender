// ============================================================================
//  cmdreg.c — SINGLE registry of console commands (see cmdreg.h)
// ----------------------------------------------------------------------------
//  All static engine commands are described here — in one table, s_cmds.
//  cmdreg_register_all() (the only registration point) is called from boot_init
//  once after console_init. Order = order in the table.
//
//  Handlers live in their own modules (commands.c, fx.c), the action matrix
//  (g_act) — in actions.c, but the command definitions are gathered here so
//  console_add_command isn't scattered across half a dozen files.
// ============================================================================
#include "cmdreg.h"

#include "actions.h"     // g_act + ActionId
#include "audio.h"       // cmd_snd_occlude
#include "attach.h"      // cmd_attach / cmd_sprite
#include "commands.h"    // action commands
#include "material.h"    // (cmd_matlist in commands.c)
#include "fx.h"          // effect commands
#include "glb.h"         // cmd_glb
#include "glb_anim.h"    // cmd_glbanim + auto-select in cmd_glb
#include "ecs.h"         // cmd_ecs_*
#include "ecs_scene.h"   // cmd_ecs_verify
#include "phys_body.h"   // cmd_phys_sphere/box/props/bodies

// --- +/- action commands (set the g_act flag) ---------------------------------
#define DEF_ACTION(id, name)                                                    \
    static void a_##name##_on(const char *a)                                    \
    { (void)a; action_set(ACTION_SRC_BIND, id, true); }                         \
    static void a_##name##_off(const char *a)                                   \
    { (void)a; action_set(ACTION_SRC_BIND, id, false); }

DEF_ACTION(A_FORWARD, forward)
DEF_ACTION(A_BACK,    back)
DEF_ACTION(A_LEFT,    left)
DEF_ACTION(A_RIGHT,   right)
DEF_ACTION(A_JUMP,    jump)
DEF_ACTION(A_DOWN,    down)
DEF_ACTION(A_SPEED,   speed)
DEF_ACTION(A_DUCK,    duck)
DEF_ACTION(A_USE,     use)

// --- Central command table -------------------------------------------------
// Adding a new command = adding one row here (and implementing
// the handler in its own module). No console_add_command calls in init code.
static const ConCmdDesc s_cmds[] = {
    // Movement (+/- commands, read by camera.c)
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

    // Particle effects (fx.c)
    { "fx",        "эффект: fx <имя> [x y z]", cmd_fx },
    { "fx_burst",  "fx_burst <имя> <x> <y> <z> [count]", cmd_fx_burst },
    { "fx_emit",   "fx_emit <имя> <x> <y> <z> <rate> [life]", cmd_fx_emit },
    { "fx_stop",   "fx_stop [имя] — остановить эмиттер(ы)", cmd_fx_stop },
    { "fx_clear",  "fx_clear — все эмиттеры + пул", cmd_fx_clear },
    { "fx_list",   "fx_list — список пресетов", cmd_fx_list },
    { "weather",   "weather <пресет> [rate] | off — погода за игроком", cmd_weather },

    // Action commands (commands.c) — available immediately, incl. in the menu
    { "toggle_map",    "показать/скрыть мини-карту", cmd_toggle_map },
    { "toggle_debug",  "показать/скрыть дебаг-панель", cmd_toggle_debug },
    { "toggle_crosshair","показать/скрыть прицел",   cmd_toggle_crosshair },
    { "toggle_shadows","вкл/выкл тени",              cmd_toggle_shadows },
    { "toggle_fog",    "вкл/выкл объёмный туман",    cmd_toggle_fog },
    { "demo",          "демо-сцена (автополёт, замер FPS)", cmd_demo },
    { "pause",         "пауза / назад в меню",       cmd_pause },
    { "map",           "map <имя> — выбрать и запустить карту", cmd_map_select },
    { "maps",          "список карт",                cmd_maps_list },
    { "noclip",        "полёт сквозь мир (чит)",     cmd_noclip },
    { "restart",       "перезапустить текущую карту", cmd_restart },
    { "changelevel",   "changelevel <карта>",        cmd_changelevel },
    { "r_drawdebug",   "r_drawdebug [0/1] — дебаг-панель", cmd_r_drawdebug },
    { "mat_wireframe", "mat_wireframe [0/1]",        cmd_mat_wireframe },
    { "thirdperson",   "вид от третьего лица",       cmd_thirdperson },
    { "firstperson",   "вид от первого лица",        cmd_firstperson },
    { "ent_create",    "ent_create <класс> [x y z]", cmd_ent_create },
    { "ent_list",      "список сущностей",           cmd_ent_list },
    { "ecs_list",      "список ECS-сущностей/компонентов/систем", cmd_ecs_list },
    { "ecs_stats",     "счётчики ECS: сущности, компоненты, системы, мост", cmd_ecs_stats },
    { "ecs_test",      "самотест ядра ECS (провалы -> FAIL в консоли)", cmd_ecs_test },
    { "ecs_verify",    "проверка связности моста сцена<->ECS", cmd_ecs_verify },
    { "snd_test",      "проиграть все звуки подряд", cmd_snd_test },
    { "snd_occlude",   "snd_occlude <x> <y> <z> — множитель глушения от слушателя до точки", cmd_snd_occlude },
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
    { "phys_rope",     "phys_rope [звеньев] — верёвка (Verlet-физика)", cmd_phys_rope },
    { "phys_push",     "phys_push [сила] — толкнуть конец верёвки",   cmd_phys_push },
    { "phys_clear",    "phys_clear — убрать физический мир",          cmd_phys_clear },
    { "phys_ragdoll",  "phys_ragdoll [hang] [kick] — рэгдолл на аним-модели", cmd_phys_ragdoll },
    { "phys_sphere",   "phys_sphere [радиус] — сфера у камеры (тела вкл)", cmd_phys_sphere },
    { "phys_box",      "phys_box [размер] — ящик у камеры (тела вкл)",   cmd_phys_box },
    { "phys_props",    "phys_props [n] — стопка ящиков (<=32) перед камерой", cmd_phys_props },
    { "phys_bodies",   "статистика тел: сколько, где, сон",              cmd_phys_bodies },
    { "glb",           "glb <glb> [x y z] [scale] — меш (.glb), скиннинг если есть", cmd_glb },
    { "glbanim",       "glbanim <glb> [x y z] [scale] — анимир. меш (skinning)", cmd_glbanim },
    { "glbclips",      "glbclips [имя-узла] — список клипов анимации", cmd_glbclips },
    { "glbclip",       "glbclip <индекс|имя> [имя-узла] — переключить клип", cmd_glbclip },
    { "glbanim_pause", "glbanim_pause [имя-узла] — пауза анимации", cmd_glbanim_pause },
    { "glbanim_play",  "glbanim_play [имя-узла] — продолжить анимацию", cmd_glbanim_play },
    { "glbanim_speed", "glbanim_speed <x> [имя-узла] — скорость анимации", cmd_glbanim_speed },
    { "glbanim_loop",  "glbanim_loop <0|1> [имя-узла] — зациклить анимацию", cmd_glbanim_loop },
    { "glbanim_seek",  "glbanim_seek <сек> [имя-узла] — поставить время клипа (с кадра)", cmd_glbanim_seek },
    { "glbanim_frame", "glbanim_frame <кадр> [fps] [имя-узла] — поставить кадр (fps=24)", cmd_glbanim_frame },
    { "anim", "anim [list|help] | anim <N> pos|move x y z [spd] | rot yaw pitch roll | clip|play|pause|loop|speed|seek|frame|on|off|scale|clips|bones|stop|remove", cmd_anim },
    { "attach", "attach sprite|glb|mesh <N> <кость|-> <файл> [...] | attach list|bones <N>", cmd_attach },
    { "sprite", "sprite <png> [x y z] [w h] [flat] — спрайт в мире", cmd_sprite },
    { "cubemap_capture", "cubemap_capture [x y z] [size] — захват кубомапа отражений (6 PNG в cubemaps/)", cmd_cubemap_capture },
    { "hp",       "hp — статус здоровья игрока",                cmd_hp },
    { "hurt",     "hurt [N] — нанести урон (по умолч. 25)",     cmd_hurt },
    { "heal",     "heal [N] — вылечить (без N — до полного)",   cmd_heal },
    { "god",      "god [0/1] — бессмертие (нужен sv_cheats)",   cmd_god },
    { "kill",     "kill — мгновенная смерть игрока",            cmd_kill },
    { "sethp",    "sethp <N> — задать HP (нужен sv_cheats)",    cmd_sethp },
    { "maxhp",    "maxhp <N> — задать максимум HP",             cmd_maxhp },
};

void cmdreg_register_all(void)
{
    for (size_t i = 0; i < sizeof s_cmds / sizeof s_cmds[0]; i++)
        console_add_command(s_cmds[i].name, s_cmds[i].help, s_cmds[i].fn);
}