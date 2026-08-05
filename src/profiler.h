// ============================================================================
//  profiler.h — профайлер: тайминги по модулям сцены (см. profiler.c)
// ----------------------------------------------------------------------------
//  Каждый кадр prof_reset() обнуляет счётчики; модули добавляют своё время
//  через prof_add_ms(). Панель рисует справа снизу (команда profiler).
//  Счётчики вершин/треугольников/draw-вызовов считаются в gl_loader.c
//  автоматически на каждом glDraw*.
// ============================================================================
#ifndef PROFILER_H
#define PROFILER_H

typedef enum {
    PROF_SKY = 0,      // скайбокс
    PROF_TERRAIN,      // рельеф (без теней)
    PROF_VEG,          // растительность
    PROF_OBJECTS,      // объекты-спрайты
    PROF_FAUNA,        // фауна
    PROF_FIELD,        // рамка поля
    PROF_SEL,          // рамки выделения
    PROF_MM,           // миникарта
    PROF_HUD,          // HUD
    PROF_SHADOW,       // каскадные тени
    PROF_FOG,          // объёмный туман
    PROF_BLOOM,        // bloom
    PROF_N
} ProfSlot;

// Обнуляет тайминги и счётчики (вызывать в начале кадра).
void prof_reset(void);

// Добавляет время (мс) в слот.
void prof_add_ms(ProfSlot s, float ms);

// --- GPU-тайминги (GL timer queries) -------------------------------------------
// Обёртка узла: prof_gpu_begin(slot) до рендера, prof_gpu_end(slot) после.
// Результат (GPU-время, мс) доступен со следующего кадра — в панели
// показывается g_prof_gpu_ms[slot] (за прошлый кадр).
void prof_gpu_begin(ProfSlot s);
void prof_gpu_end(ProfSlot s);
float prof_gpu_ms(ProfSlot s);

const char *prof_name(ProfSlot s);
float prof_ms(ProfSlot s);

// Сумма времени всех 3D-слотов (для процентов).
float prof_total_ms(void);

#endif
