// ============================================================================
//  material.h — материал поверхности: единое описание отображения спрайта
// ----------------------------------------------------------------------------
//  Доведённый MapObjectRender (п.14): blend/cull/depth_write/wrap/filter/
//  tint/alpha/UV-анимация — ДАННЫЕ, а не код в render-цикле. Состояние GL
//  и uniform'ы шейдера применяет material_bind (паттерн «ребёнок снабжает
//  шейдер uniform-ами»): новая текстурная/UV-фича добавляется в Material +
//  material_bind — render-цикл objects.c не трогается.
//
//  Дефолты (для объектов без блока material в JSON) задаёт material_defaults.
// ============================================================================
#ifndef MATERIAL_H
#define MATERIAL_H
#include "gl_loader.h"

typedef enum {
    MAT_BLEND_ALPHATEST = 0,   // альфа-тест: прозрачный фон отбрасывается (по умолч.)
    MAT_BLEND_ALPHA,           // обычный блендинг (полупрозрачность)
    MAT_BLEND_ADDITIVE,        // аддитивный (свечение, огонь)
    MAT_BLEND_OPAQUE           // непрозрачный (альфа игнорируется)
} MatBlend;

typedef enum {
    MAT_CULL_NONE = 0,         // двусторонний (по умолчанию)
    MAT_CULL_BACK,             // отсекать заднюю сторону
    MAT_CULL_FRONT             // отсекать переднюю
} MatCull;

typedef enum {
    MAT_WRAP_CLAMP = 0,        // по краям — цвет края (по умолчанию)
    MAT_WRAP_REPEAT,           // повторение
    MAT_WRAP_MIRROR            // зеркальное повторение
} MatWrap;

typedef enum {
    MAT_FILTER_LINEAR = 0,     // сглаживание (по умолчанию)
    MAT_FILTER_NEAREST,        // пиксельный (ретро, спрайты)
    MAT_FILTER_MIPMAP          // мип-уровни (дальние объекты чёткие)
} MatFilter;

typedef struct {
    float tint[3];       // цветовой тон (1 = без изменений)
    float alpha;         // прозрачность 0..1 (для alpha/additive)
    int blend;           // MatBlend
    int cull;            // MatCull
    int depth_write;     // 1 — писать в буфер глубины
    int wrap;            // MatWrap (требует свою текстуру)
    int filter;          // MatFilter
    float tile[2];       // повтор UV (1 = один тайл текстуры)
    float offset[2];     // сдвиг UV в тайлах
    float scroll[2];     // анимация UV: тайлов в секунду
    int fixed_size;      // 1 — ширина/высота в метрах (не из текстуры)
    float width, height; // размер квада, м (если fixed_size)
    int set;             // блок material был в JSON
} Material;

// Дефолты материала (для объектов без блока material).
void material_defaults(Material *m);

// Хендлы uniform'ов материала в шейдере (ищутся один раз; необязательные = -1).
typedef struct {
    GLint u_tint, u_blend, u_alpha;
    GLint u_uvtile, u_uvoffset, u_uvscroll, u_time;
} MaterialLocs;

void material_locs_init(MaterialLocs *ml, GLuint prog);

// Применяет материал: состояние GL (блендинг/отсечение/глубина) и uniform'ы
// (tint/alpha/blend/UV-анимация). Вызывать на каждый инстанс после
// биндинга программы. time — время (для scroll-анимации).
void material_bind(const Material *m, const MaterialLocs *ml, float time);

#endif
