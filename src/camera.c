// ============================================================================
//  camera.c — камера: ввод (WASD + мышь), физика ходьбы, матрицы вида/проекции
// ============================================================================
#include "camera.h"

#include "actions.h"
#include "audio.h"
#include "cvars.h"
#include "linalg.h"
#include "world.h"
#include "objects.h"
#include "water.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Счётчик сыгранных шагов (для snd_debug в главном цикле)
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

void camera_dirs(const Camera *cam, float *forward, float *right)
{
    float cp = cosf(cam->pitch);
    forward[0] = sinf(cam->yaw) * cp;
    forward[1] = sinf(cam->pitch);
    forward[2] = cosf(cam->yaw) * cp;
    if (right) {
        right[0] = -forward[2];   // cross(forward, up) = (-f.z, 0, f.x)
        right[1] = 0.0f;
        right[2] = forward[0];
    }
}

void camera_update(Camera *cam, const WorldState *w,
                    float dx, float dy, float dt)
{
    // Параметры движения — cvar'ы из консоли (set cl_sens 0.01 и т.п.)
    float sens = cvar_get("cl_sens");
    if (sens <= 0.0f) sens = MOUSE_SENS;

    // Мышь вправо (dx>0) — поворачиваем взгляд вправо. В нашей системе
    // yaw растёт ВЛЕВО (forward = (sin yaw, 0, cos yaw)), поэтому минус.
    // m_invert — инверсия вертикали (меню: Управление).
    // Интерполяция взгляда (cvar m_smooth, 0..1): 0 — сырое движение как
    // раньше, 1 — сильное сглаживание. Целевые yaw/pitch копят ввод, а
    // текущий взгляд догоняет их экспоненциально (независимо от FPS).
    float inv = cvar_get("m_invert") > 0.5f ? -1.0f : 1.0f;
    static float s_want_yaw = 0.0f, s_want_pitch = 0.0f;
    static int s_smooth_init = 0;
    float lim = 3.14159f / 2.0f - 0.01f;
    if (!s_smooth_init) {
        s_want_yaw = cam->yaw;
        s_want_pitch = cam->pitch;
        s_smooth_init = 1;
    }
    // Телепорт/новая игра/загрузка: взгляд сбросили извне — догонять
    // старую цель нельзя (поедет камера). Привязываем цели к текущему.
    if (fabsf(cam->yaw - s_want_yaw) > 3.0f ||
        fabsf(cam->pitch - s_want_pitch) > 3.0f) {
        s_want_yaw = cam->yaw;
        s_want_pitch = cam->pitch;
    }
    s_want_yaw  -= dx * sens;
    s_want_pitch -= dy * sens * inv;       // мышь вверх — смотрим вверх
    if (s_want_pitch >  lim) s_want_pitch =  lim;
    if (s_want_pitch < -lim) s_want_pitch = -lim;
    float smooth = cvar_get("m_smooth");
    if (smooth < 0.0f) smooth = 0.0f;
    if (smooth > 1.0f) smooth = 1.0f;
    if (smooth < 0.001f) {                 // 0 — мгновенно, как раньше
        cam->yaw = s_want_yaw;
        cam->pitch = s_want_pitch;
    } else {
        // Доля приближения за кадр на 60 FPS: smooth=1 -> 0.05 («дикая»
        // интерполяция, сильный дрейф), smooth->0 -> ~1 (почти мгновенно)
        float k = 0.05f + 0.95f * (1.0f - smooth);
        float f = 1.0f - powf(1.0f - k, 60.0f * dt);
        cam->yaw   += (s_want_yaw  - cam->yaw) * f;
        cam->pitch += (s_want_pitch - cam->pitch) * f;
    }
    if (cam->pitch >  lim) cam->pitch =  lim;   // не даём уйти "за голову"
    if (cam->pitch < -lim) cam->pitch = -lim;

    float fwd[3], right[3];
    camera_dirs(cam, fwd, right);
    // Движение — через действия из биндов (+forward/-forward и т.п.)
    // noclip летает со скоростью полёта (как старый fly-контроллер)
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

// --- Raycast: луч из камеры вниз до рельефа --------------------------------
// Возвращает 1 если луч попал в рельеф, 0 если вышел за мир.
// out_x, out_z — точка попадания; out_y — высота рельефа в точке.
int camera_raycast_terrain(const Camera *cam, const WorldState *w,
                           float max_dist, float *out_x, float *out_y, float *out_z)
{
    if (!cam || !w) return 0;
    float fwd[3];
    camera_dirs(cam, fwd, NULL);
    float x = cam->pos[0];
    float y = cam->pos[1];
    float z = cam->pos[2];
    float step = 0.2f;   // шаг марша (м)
    float dist = 0.0f;
    while (dist < max_dist) {
        x += fwd[0] * step;
        y += fwd[1] * step;
        z += fwd[2] * step;
        dist += step;
        // Проверка выхода за мир
        if (x < 1.0f || x > w->world_w - 1.0f ||
            z < 1.0f || z > w->world_d - 1.0f)
            return 0;
        float h = sample_height(w, x, z);
        if (y <= h + 0.1f) {
            if (out_x) *out_x = x;
            if (out_y) *out_y = h;
            if (out_z) *out_z = z;
            return 1;
        }
    }
    return 0;
}

SceneNode *camera_make_node(const WorldState *w, const float *start_pos,
                            float yaw, float pitch,
                            float fov_deg, float znear, float zfar)
{
    CameraParams *p = calloc(1, sizeof *p);
    p->world = w;
    p->cam.pos[0] = start_pos[0];
    p->cam.pos[1] = start_pos[1];
    p->cam.pos[2] = start_pos[2];
    p->cam.yaw = yaw;
    p->cam.pitch = pitch;
    p->fov_deg = fov_deg;
    p->znear = znear;
    p->zfar = zfar;
    const float size[3] = { fov_deg, znear, zfar };   // fov, near, far — тоже "размер"
    return node_new("Camera", "Camera3D", start_pos, size, p,
                    camera_node_update, NULL);
}

static int s_demo = 0;          // демо-сцена: ввод выключен, камеру ведёт main.c
static int s_input_lock = 0;    // консоль открыта: ввод игрока выключен

void camera_demo_set(int on)
{
    s_demo = on;
}

void camera_lock_input(int lock)
{
    s_input_lock = lock;
}

void camera_node_update(SceneNode *n, float dt)
{
    CameraParams *p = n->params;
    player_sounds_load();   // ленивая загрузка звуков игрока
    // FOV — cvar (консоль: set fov 90, колесо мыши тоже пишет в cvar)
    float fov = cvar_get("fov");
    if (fov > 0.0f && fov <= 180.0f) p->fov_deg = fov;
    if (!s_demo && !s_input_lock) {              // в демо/консоли камеру не трогаем
        float mx = 0, my = 0;
        if (!getenv("FR_NOMOUSE"))                // детерминированные тесты
            SDL_GetRelativeMouseState(&mx, &my);  // мышь — обзор
        camera_update(&p->cam, p->world, mx, my, dt);
    }

    // Матрицы вида и проекции. Вид от третьего лица (cvar cam_thirdperson):
    // chase-камера — стоит на сфере радиуса cam_tpd вокруг головы персонажа
    // вдоль НАПРАВЛЕНИЯ ВЗГЛЯДА (включая pitch: камера крутится и по
    // вертикали); цель — голова персонажа. Взгляд камеры = направление
    // взгляда игрока, поэтому sky/биллборды в renderer читают реальный
    // видовой базис.
    float fwd[3], up[3] = {0, 1, 0}, center[3], view[16], proj[16];
    camera_dirs(&p->cam, fwd, NULL);
    center[0] = p->cam.pos[0] + fwd[0];
    center[1] = p->cam.pos[1] + fwd[1];
    center[2] = p->cam.pos[2] + fwd[2];
    if (cvar_get("cam_thirdperson") > 0.5f) {
        float dist = cvar_get("cam_tpd");   // дистанция камеры, м
        if (dist < 1.0f) dist = 4.0f;
        if (dist > 12.0f) dist = 12.0f;
        // Голова персонажа — центр orbита (чуть выше точки глаз)
        float head[3] = {
            p->cam.pos[0],
            p->cam.pos[1] + 0.3f,
            p->cam.pos[2],
        };
        // Камера — на сфере взгляда: pos - fwd*dist (полный 3D-взгляд,
        // поэтому мышь вверх/вниз реально крутит камеру вокруг персонажа).
        float eye[3] = {
            head[0] - fwd[0] * dist,
            head[1] - fwd[1] * dist,
            head[2] - fwd[2] * dist,
        };
        // Камера не проваливается в рельеф (на склонах/у обрывов)
        float ground = sample_height(p->world, eye[0], eye[2]) + 0.35f;
        if (eye[1] < ground) eye[1] = ground;
        mat4_lookat(view, eye, head, up);
        // Действительный видовой базис (откуда реально смотрит рендер)
        memcpy(p->view_pos, eye, sizeof p->view_pos);
    } else {
        mat4_lookat(view, p->cam.pos, center, up);
        memcpy(p->view_pos, p->cam.pos, sizeof p->view_pos);
    }
    // Базис ВИДОВОЙ камеры — из готовой матрицы вида (column-major):
    // right = (m0, m4, m8), up = (m1, m5, m9), forward = -(m2, m6, m10).
    // В 1-м лице он совпадает с camera_dirs, в 3-м лице — это реальное
    // направление рендера (небо/биллборды/лучи берут его, а не взгляд игрока).
    p->view_right[0] = view[0]; p->view_right[1] = view[4]; p->view_right[2] = view[8];
    p->view_up[0]    = view[1]; p->view_up[1]    = view[5]; p->view_up[2]    = view[9];
    p->view_fwd[0]   = -view[2]; p->view_fwd[1]  = -view[6]; p->view_fwd[2]  = -view[10];
    mat4_perspective(proj, p->fov_deg * 3.14159f / 180.0f, p->aspect, p->znear, p->zfar);
    mat4_mul(p->viewproj, proj, view);
}
