// ============================================================================
//  movement.c — физика/движение игрока (см. movement.h)
// ----------------------------------------------------------------------------
//  Перенесено из camera.c: всё, что не про взгляд — ходьба (гравитация,
//  земля, коллизия объектов карты, вода/плавучесть), полёт (fly), проход
//  сквозь (noclip) и звуки игрока (шаги/прыжок/приземление/шелест). Камера
//  (вид) осталась в camera.c; здесь только движение и его побочные эффекты.
// ============================================================================
#include "movement.h"

#include "actions.h"
#include "audio.h"
#include "cvars.h"
#include "linalg.h"     // M_PI для расчёта крутизны склона
#include "objects.h"    // objects_collide — коллизия с объектами карты
#include "phys_body.h"  // phys_body_push — игрок толкает твёрдые тела
#include "water.h"      // water_surface_at / water_flow_at

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// --- Звуки игрока (шаги/прыжок/приземление) ------------------------------------
// Грузятся лениво, через ресурсы: карта может переопределить их из zip.
static Sound *s_step[3] = { NULL, NULL, NULL };
static Sound *s_land = NULL;
static Sound *s_rustle[3] = { NULL, NULL, NULL };  // три варианта шелеста
static Sound *s_jump = NULL;
static int s_rustle_sel = 0;
static int s_step_tried = 0;
static int s_step_sel = 0;
static float s_step_timer = 0.0f;
static float s_rustle_timer = 0.0f;
static int s_was_air = 0;    // был ли в воздухе в прошлом кадре (приземление)
static int s_prev_duck = 0;  // приседали ли в прошлом кадре (смена — не падение)
static float s_fall_impact = 0.0f;  // скорость падения в момент приземления (м/с)

float movement_take_fall_impact(void)
{
    float v = s_fall_impact;
    s_fall_impact = 0.0f;
    return v;
}

// Позиция игрока (слушатель = камера; источник звуков — игрок)
#define SOUND_RADIUS 25.0f    // радиус слышимости звуков игрока, м
static float s_player_pos[3] = { 0, 0, 0 };

static void player_sounds_load(void)
{
    if (s_step_tried) return;
    s_step_tried = 1;
    for (int i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof p, "audio/step%d.wav", i + 1);
        s_step[i] = audio_load(p);
    }
    s_land = audio_load("audio/land.wav");
    for (int i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof p, "audio/rustle%d.wav", i + 1);
        s_rustle[i] = audio_load(p);
    }
    s_jump = audio_load("audio/jump.wav");
}

// Счётчик сыгранных шагов (для snd_debug в renderer.c)
int g_step_calls = 0;

static void player_step_sound(void)
{
    if (!s_step[0] && !s_step[1] && !s_step[2]) return;
    Sound *s = s_step[s_step_sel++ % 3];
    // Присед — тише шаги. Позиционно: источник на месте игрока.
    float vol = g_act[A_DUCK] ? 0.25f : 0.5f;
    g_step_calls++;
    audio_3d_play_once(s, vol, s_player_pos, SOUND_RADIUS);
}

void movement_update(Camera *cam, const WorldState *w, float dt)
{
    if (!cam || !w) return;
    player_sounds_load();

    float fwd[3], right[3];
    camera_dirs(cam, fwd, right);

    // Параметры движения — cvar'ы из консоли (set cl_walkspeed 5 и т.п.)
    float maxspeed = cvar_get((cam->fly || cam->noclip) ? "cl_flightspeed"
                                                        : "cl_walkspeed");
    if (!cam->fly && !cam->noclip) {   // sv_maxspeed перекрывает ходьбу (HL-стиль)
        float ms = cvar_get("sv_maxspeed");
        if (ms > 0.0f) maxspeed = ms;
    }
    if (maxspeed <= 0.0f) maxspeed = (cam->fly || cam->noclip)
                                   ? CAM_SPEED : WALK_SPEED;
    if (g_act[A_SPEED]) maxspeed *= 4.0f;          // +speed — бег
    float speed = maxspeed * dt;

    // NOCLIP: коллизии персонажа временно выключены. Отдельный контроллер,
    // как у полёта (fly): свободное перемещение ПО ВЗГЛЯДУ (вперёд/назад с
    // вертикальной компонентой), Space/Ctrl — вверх/вниз. Гравитации, земли
    // и объектов нет. Положение НЕ телепортируется — при выключении noclip
    // персонаж остаётся там, где был, и снова подчиняется коллизии ходьбы.
    if (cam->noclip) {
        if (g_act[A_FORWARD]) { cam->pos[0] += fwd[0]*speed; cam->pos[1] += fwd[1]*speed; cam->pos[2] += fwd[2]*speed; }
        if (g_act[A_BACK])    { cam->pos[0] -= fwd[0]*speed; cam->pos[1] -= fwd[1]*speed; cam->pos[2] -= fwd[2]*speed; }
        if (g_act[A_RIGHT])   { cam->pos[0] += right[0]*speed; cam->pos[2] += right[2]*speed; }
        if (g_act[A_LEFT])    { cam->pos[0] -= right[0]*speed; cam->pos[2] -= right[2]*speed; }
        if (g_act[A_JUMP]) cam->pos[1] += speed;
        if (g_act[A_DOWN]) cam->pos[1] -= speed;
        cam->vx = cam->vz = 0; cam->vy = 0;   // ходьба стартует с нуля
        s_was_air = 0;
    } else

    if (cam->fly) {
        // --- полёт: свободное перемещение по взгляду ---
        if (g_act[A_FORWARD]) { cam->pos[0] += fwd[0]*speed; cam->pos[1] += fwd[1]*speed; cam->pos[2] += fwd[2]*speed; }
        if (g_act[A_BACK])    { cam->pos[0] -= fwd[0]*speed; cam->pos[1] -= fwd[1]*speed; cam->pos[2] -= fwd[2]*speed; }
        if (g_act[A_RIGHT])   { cam->pos[0] += right[0]*speed; cam->pos[2] += right[2]*speed; }
        if (g_act[A_LEFT])    { cam->pos[0] -= right[0]*speed; cam->pos[2] -= right[2]*speed; }
        if (g_act[A_JUMP]) cam->pos[1] += speed;
        if (g_act[A_DOWN]) cam->pos[1] -= speed;
    } else {
        // --- ходьба: контроллер в духе Half-Life 1 ---
        // Приседание (+duck): ниже глаза, медленнее, тише шаги.
        // Смена присед/встать МГНОВЕННО ставит камеру на землю — это
        // НЕ падение: звук приземления не играет.
        bool duck = g_act[A_DUCK];
        float eye = duck ? EYE_DUCK : EYE_H;
        float ground = sample_height(w, cam->pos[0], cam->pos[2]) + eye;
        // За краем карты земли нет: игрок летит вниз (без невидимой стены,
        // sample_height иначе клэмпил высоту до края — игрок «висел» над краем)
        bool offmap = cam->pos[0] < 0.0f || cam->pos[0] > w->world_w ||
                      cam->pos[2] < 0.0f || cam->pos[2] > w->world_d;
        if (offmap) ground = -1e30f;
        if (duck != s_prev_duck) {
            s_prev_duck = duck;
            if (ground > -1e29f) {     // за краем карты мгновенно не садимся
                cam->pos[1] = ground;    // мгновенно сесть/встать
                cam->vy = 0;
            }
            s_was_air = 0;
        }
        bool on_ground = cam->pos[1] <= ground + 0.01f && cam->vy <= 0.01f;
        if (duck) maxspeed *= 0.5f;          // в приседе медленнее

        float wishx = 0, wishz = 0;
        if (g_act[A_FORWARD]) { wishx += fwd[0]; wishz += fwd[2]; }
        if (g_act[A_BACK])    { wishx -= fwd[0]; wishz -= fwd[2]; }
        if (g_act[A_RIGHT])   { wishx += right[0]; wishz += right[2]; }
        if (g_act[A_LEFT])    { wishx -= right[0]; wishz -= right[2]; }
        float wlen = sqrtf(wishx*wishx + wishz*wishz);
        bool want_move = wlen > 1e-4f;
        if (want_move) { wishx /= wlen; wishz /= wlen; }

        // Твёрдый рельеф (sv_maxslope): склон круче maxкрутизны — как стена.
        // Скользим вдоль контура равной высоты вместо лезния вверх по круче склону.
        if (want_move && !cam->fly) {
            float max_slope = cvar_get("sv_maxslope");
            if (max_slope > 0.0f && max_slope < 90.0f) {
                float e = 0.75f;   // радиус сэмпла градиента, м
                float hx = sample_height(w, cam->pos[0] + e, cam->pos[2])
                         - sample_height(w, cam->pos[0] - e, cam->pos[2]);
                float hz = sample_height(w, cam->pos[0], cam->pos[2] + e)
                         - sample_height(w, cam->pos[0], cam->pos[2] - e);
                float glen = sqrtf(hx*hx + hz*hz);          // |градиент высоты|
                float slope_sin = glen / sqrtf(glen*glen + 4.0f*e*e); // sin угла наклона
                float step_up = wishx*hx + wishz*hz;        // идём ли вверх по склону
                if (step_up > 0.0f && slope_sin > sinf(max_slope * (float)M_PI / 180.0f)) {
                    // движение вверх по слишком крутому склону: спроецируем
                    // вдоль контура равной высоты (перпендикулярно градиенту)
                    float cx = -hz, cz = hx;                // вдоль контура
                    float clen = sqrtf(cx*cx + cz*cz);
                    if (clen > 1e-5f) {
                        cx /= clen; cz /= clen;
                        float along = wishx*cx + wishz*cz;
                        wishx = cx * along;
                        wishz = cz * along;
                        wlen = fabsf(along);
                    } else {
                        wishx = wishz = 0.0f;
                        wlen = 0.0f;
                    }
                    want_move = wlen > 1e-4f;
                }
            }
        }

        // ускорение к желаемой скорости (в воздухе — слабее)
        if (want_move) {
            float dot = cam->vx*wishx + cam->vz*wishz;
            float add = maxspeed - dot;
            if (add > 0.0f) {
                float a = (on_ground ? 8.0f : 2.5f) * maxspeed * dt;
                if (a > add) a = add;
                cam->vx += wishx * a;
                cam->vz += wishz * a;
            }
        } else if (on_ground) {
            // трение о землю
            float fr = 1.0f - 6.0f * dt;
            if (fr < 0.0f) fr = 0.0f;
            cam->vx *= fr;
            cam->vz *= fr;
        }
        // потолок скорости
        float sp2 = cam->vx*cam->vx + cam->vz*cam->vz;
        if (sp2 > maxspeed*maxspeed) {
            float s = maxspeed / sqrtf(sp2);
            cam->vx *= s; cam->vz *= s;
        }
        cam->pos[0] += cam->vx * dt;
        cam->pos[2] += cam->vz * dt;
        // Коллизии объектов карты (collision=1): не проходить сквозь них.
        // Радиус персонажа = половина ширины/глубины коллизии (1 м -> 0.5 м,
        // см. camera.h PLAYER_HALF_W); пшеница раздвигается отдельно в wheat.vs.
        objects_collide(&cam->pos[0], &cam->pos[2], PLAYER_HALF_W);

        // Тела (phys_body): игрок толкает сферу/ящики поясом — импульс от
        // горизонтальной скорости. Только в ветке ходьбы: в fly/noclip
        // коллизий нет по контракту (см. src/movement.c:107).
        {
            float vel[3] = { cam->vx, 0.0f, cam->vz };
            float lim = cvar_get("phys_push_speed");
            if (lim <= 0.0f) lim = 4.0f;
            phys_body_push(cam->pos, vel, PLAYER_HALF_W, lim);
        }

        float gravity = cvar_get("sv_gravity");
        if (gravity <= 0.0f) gravity = GRAVITY;
        cam->vy -= gravity * dt;                 // гравитация

        // Вода: если точка покрыта водоёмом и глаза под поверхностью
        // (вода глубже роста) — плаваем. Плавучесть выталкивает к
        // поверхности, а течение реки (water_flow_at) сносит игрока.
        float wl = water_surface_at(w, cam->pos[0], cam->pos[2]);
        if (offmap) wl = -1e30f;       // за картой воды тоже нет
        bool in_water = wl > ground - 0.05f;     // поверхность выше "дна"
        if (in_water && cam->pos[1] < wl - 0.05f) {   // глаза под водой
            // Плавучесть: под водой гравитация гасится, выталкивание
            // превышает её (24 > 20) — игрок всплывает к поверхности,
            // а не тонет на дно (там вода была не видна "вблизи").
            cam->vy += (24.0f - gravity * 0.5f) * dt;
            cam->vy *= 0.98f;                    // вязкое сопротивление
            float flo[2];
            if (water_flow_at(w, cam->pos[0], cam->pos[2], flo)) {
                cam->vx += flo[0] * 3.5f * dt;   // течение реки несёт
                cam->vz += flo[1] * 3.5f * dt;
            }
        }

        float ny = cam->pos[1] + cam->vy * dt;
        if (in_water && ny > wl) ny = wl;        // не выныривать в воздух
        if (ny <= ground) {                      // приземлились (или уже стоим)
            float fall = cam->vy;
            cam->pos[1] = ground;
            cam->vy = 0;
            float jump = cvar_get("sv_jumpspeed");
            if (jump <= 0.0f) jump = JUMP_V;
            if (g_act[A_JUMP]) {
                cam->vy = jump;                  // прыжок
                if (s_jump)
                    audio_3d_play_once(s_jump, 0.3f, s_player_pos, SOUND_RADIUS);
            } else if (s_was_air && fall < -1.5f) {
                // Приземление после падения с высоты: звук ВСЕГДА,
                // громкость зависит от скорости падения.
                if (s_land) {
                    float vol = 0.2f + (-fall) / 14.0f;
                    if (vol > 0.6f) vol = 0.6f;
                    audio_3d_play_once(s_land, vol, s_player_pos, SOUND_RADIUS);
                }
                // Скорость падения -> игровой слой (урон от падения, game.c).
                s_fall_impact = -fall;
            }
            s_was_air = 0;
        } else {
            cam->pos[1] = ny;                    // летим/падаем
            s_was_air = 1;
        }

        // Шаги: движение по земле (не в полёте, не падаем). Таймер зависит
        // от скорости: бег — чаще. Присед — тише.
        float vsp2 = cam->vx * cam->vx + cam->vz * cam->vz;
        if (!in_water && cam->vy <= 0.1f && vsp2 > 0.5f) {
            s_step_timer -= dt;
            if (s_step_timer <= 0.0f) {
                float spd = sqrtf(vsp2);
                s_step_timer = (spd > 1.5f) ? 0.34f : 0.5f;
                player_step_sound();
            }
        } else {
            s_step_timer = 0.0f;
        }

        // Шуршание: только при движении сквозь пшеницу (поле). Трава/кусты
        // звука не дают. Три варианта шелеста чередуются (как шаги).
        if (!in_water && cam->vy <= 0.1f && vsp2 > 0.5f) {
            bool in_field =
                cam->pos[0] >= w->field_x0 && cam->pos[0] <= w->field_x0 + w->field_w &&
                cam->pos[2] >= w->field_z0 && cam->pos[2] <= w->field_z0 + w->field_d;
            if (in_field) {
                s_rustle_timer -= dt;
                if (s_rustle_timer <= 0.0f) {
                    s_rustle_timer = 0.45f;
                    Sound *s = s_rustle[s_rustle_sel++ % 3];
                    if (s)
                        audio_3d_play_once(s, 0.32f, s_player_pos, SOUND_RADIUS);
                }
            } else {
                s_rustle_timer = 0.0f;
            }
        } else {
            s_rustle_timer = 0.0f;
        }
    }
    // позиция игрока — для позиционных звуков (шаги и т.п.)
    s_player_pos[0] = cam->pos[0];
    s_player_pos[1] = cam->pos[1];
    s_player_pos[2] = cam->pos[2];
}