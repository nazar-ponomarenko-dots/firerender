// ============================================================================
//  camera.c — камера: ввод (WASD + мышь), физика ходьбы, матрицы вида/проекции
// ============================================================================
#include "camera.h"

#include "cvars.h"
#include "linalg.h"
#include "movement.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Цели интерполяции взгляда (m_smooth) — в области модуля, чтобы
// camera_sync_look мог привязать их к текущему взгляду при телепорте.
static float s_want_yaw = 0.0f, s_want_pitch = 0.0f;
static int s_smooth_init = 0;

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
    // Цели в области модуля: camera_sync_look (телепорты) привязывает их
    // к текущему взгляду напрямую.
    float inv = cvar_get("m_invert") > 0.5f ? -1.0f : 1.0f;
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

    // Физика/движение (ходьба, полёт, noclip, вода, звуки игрока) — вынесена
    // в отдельный модуль movement.c, чтобы движение не было зашито в камеру.
    movement_update(cam, w, dt);
}

// Привязать цели сглаживания к ТЕКУЩЕМУ взгляду. Вызывать после прямой
// записи yaw/pitch (телепорт, загрузка, спавн): иначе m_smooth потянет
// взгляд обратно к старой цели — даже при малой дельте, не доходящей до
// эвристики «>3 рад» в camera_update.
void camera_sync_look(Camera *cam)
{
    if (!cam) return;
    s_want_yaw = cam->yaw;
    s_want_pitch = cam->pitch;
    s_smooth_init = 1;
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
