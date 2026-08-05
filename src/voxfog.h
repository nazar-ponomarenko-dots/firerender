// ============================================================================
//  voxfog.h — воксельный объёмный туман (каскады 3D-текстур)
// ----------------------------------------------------------------------------
//  Как тени, но в 3D: вокруг камеры — несколько 3D-воксельных каскадов
//  (ближний 16 м / средний 48 м / дальний 160 м), плотность тумана
//  считается на CPU из рельефа и поля пшеницы. Рендер — ОДИН
//  полноэкранный проход (deferred): по лучу из камеры в глубину сцены
//  маршируются каскады, туман ложится на всё (земля, растения, деревья,
//  небо) автоматически. Клавиша P — вкл/выкл.
// ============================================================================
#ifndef VOXFOG_H
#define VOXFOG_H
#include "gl_loader.h"
#include "camera.h"
#include "sun.h"
#include "world.h"

#define VF_CASCADES 3
#define VF_BOX0 16.0f      // сторона каскада 0, м
#define VF_BOX1 48.0f
#define VF_BOX2 160.0f
#define VF_RES0 32         // разрешение каскадов (voxel 0.5 / 1 / 5 м)
#define VF_RES1 48
#define VF_RES2 32         // дальний — ГРУБЕЕ (мягкий, фпс дороже чёткости)

// Создаёт 3D-текстуры каскадов и FBO полноэкранного прохода.
// w — данные мира (рельеф и поле: плотность тумана).
void voxfog_init(const WorldState *w, int vw, int vh);

// Пересоздаёт FBO при изменении размера окна.
void voxfog_resize(int vw, int vh);

// Каждый кадр: каскады привязаны к МИРУ (центр — с шагом вокселя), пере-
// строится только каскад, чей якорь сменился. Быстро.
void voxfog_update(const float cam_pos[3]);

// Пайплайн с туманом: сцена -> bloom (композит пишется В voxfog_fbo) ->
// voxfog_render (туман поверх bloom'а, прямо на экран — bloom его не
// подсвечивает). Без тумана: bloom_render(scene, 0) как раньше.
GLuint voxfog_fbo(void);            // куда bloom пишет композит
GLuint voxfog_texture(void);        // этот композит (текстура)

// Полноэкранный проход: композит bloom'а (цвет) + глубина сцены + марш
// по каскадам -> экран. ПОЛНОЕ разрешение.
void voxfog_render(int vw, int vh, const CameraParams *cam,
                   const SunParams *sun, GLuint bloomed_tex, GLuint scene_depth);

int  voxfog_is_on(void);
void voxfog_toggle(void);
void voxfog_set(int on);   // принудительно (FR_TEST)
// Режимы тумана (r_fog_mode):
//   0 = выкл, 1 = объёмный (3D-каскады), 2 = дешёвый (screen-space
//   высотный туман, один полноэкранный проход — для слабых машин).
void voxfog_set_mode(int mode);
int  voxfog_mode(void);

void voxfog_cleanup(void);

#endif
