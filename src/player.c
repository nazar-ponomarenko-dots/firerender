// ============================================================================
//  player.c — состояние игрока (см. player.h)
// ============================================================================
#include "player.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

void player_init(Player *p)
{
    memset(p, 0, sizeof *p);
    p->spawn_yaw = 3.14159f;
    p->spawn_pitch = -0.5f;
}

void player_set_spawn(Player *p, const float pos[3], float yaw, float pitch)
{
    if (pos) {
        p->spawn_pos[0] = pos[0];
        p->spawn_pos[1] = pos[1];
        p->spawn_pos[2] = pos[2];
    }
    p->spawn_yaw = yaw;
    p->spawn_pitch = pitch;
}

void player_teleport(Player *p, Camera *cam, const float pos[3],
                     float yaw, float pitch)
{
    (void)p;
    if (!cam || !pos) return;
    cam->pos[0] = pos[0];
    cam->pos[1] = pos[1];
    cam->pos[2] = pos[2];
    cam->yaw = yaw;
    cam->pitch = pitch;
    cam->vx = cam->vy = cam->vz = 0.0f;
    cam->fly = 0;
    // Цели сглаживания мыши — к новому взгляду: иначе m_smooth тянет камеру
    // обратно к старой точке (заметно при загрузке/спавне с малой дельтой,
    // не доходящей до эвристики «>3 рад» в camera_update).
    camera_sync_look(cam);
    SDL_Log("Teleport: %.1f %.1f %.1f yaw=%.2f pitch=%.2f",
            pos[0], pos[1], pos[2], yaw, pitch);
}

void player_save(Player *p, const Camera *cam, const char *map_name)
{
    if (!cam) return;
    p->save_pos[0] = cam->pos[0];
    p->save_pos[1] = cam->pos[1];
    p->save_pos[2] = cam->pos[2];
    p->save_yaw = cam->yaw;
    p->save_pitch = cam->pitch;
    if (map_name) snprintf(p->save_map, sizeof p->save_map, "%s", map_name);
}

void player_load(Player *p, float x, float y, float z,
                 float yaw, float pitch, const char *map_name)
{
    p->save_pos[0] = x;
    p->save_pos[1] = y;
    p->save_pos[2] = z;
    p->save_yaw = yaw;
    p->save_pitch = pitch;
    if (map_name) snprintf(p->save_map, sizeof p->save_map, "%s", map_name);
}