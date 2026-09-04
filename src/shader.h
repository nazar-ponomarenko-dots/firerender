// ============================================================================
//  shader.h — загрузка и компиляция GLSL-шейдеров из папки shaders/
// ----------------------------------------------------------------------------
//  Версия GLSL и точность (precision) для GLES зависят от платформы, поэтому
//  они подставляются кодом (см. compile_shader_file), а в файлах шейдеров —
//  только тело.
// ============================================================================
#ifndef SHADER_H
#define SHADER_H
#include "gl_loader.h"
#include "resources.h"   // ReshadeFn (hot-reload)

// Компилирует шейдер из файла; при ошибке печатает лог. Возвращает ID или 0.
GLuint compile_shader_file(GLenum type, const char *path);

// Связывает вершинный + фрагментный шейдер в программу (кеш в resources.c).
GLuint make_program(const char *vs_path, const char *fs_path);

// Компилирует и линкует ВЫЧИСЛИТЕЛЬНЫЙ шейдер (.cs) в программу.
// Используется GPU-driven cull-проходом растительности (GL 4.3+). Без кеша.
// Возвращает ID или 0. Привязку SSBO-блоков к binding point делает вызывающий
// через glShaderStorageBlockBinding(prog, ...).
GLuint make_compute_program(const char *cs_path);

// --- Hot-reload шейдеров --------------------------------------------------------
// Регистрация хука пересборки: после перекомпиляции изменённых программ
// модуль повторно ищет uniform'ы и статичные uniform'ы (см. resources.c).
void shader_on_reshade(ReshadeFn fn);

// Перекомпилирует изменённые шейдеры (по mtime файлов) и зовёт хуки.
// Вызывать из главного цикла (раз в ~30 кадров) и командой reload_shaders.
int shader_hot_reload(void);

#endif
