// ============================================================================
//  resources.c — реализация менеджера ресурсов (см. resources.h)
// ----------------------------------------------------------------------------
//  Один общий кеш (связный список): картинки (пиксели), текстуры и шейдерные
//  программы. Ключ — строка, у текстур/программ ещё и счётчик ссылок.
// ============================================================================
#include "resources.h"

#include "assets.h"      // make_texture
#include "miniz/miniz.h" // zip-паки карт
#include "shader.h"      // compile_shader_file

#define STB_IMAGE_IMPLEMENTATION   // реализация stb_image — ровно здесь
#include "stb_image.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// --- Пак карты (maps/*.zip) ----------------------------------------------------
static mz_zip_archive s_pak;
static int s_pak_open = 0;

// Очистка кеша от ресурсов карты (объявлена ниже, используется в pak_close)
static void cache_purge_pak(void);

int resources_pak_open(const char *path)
{
    if (s_pak_open) resources_pak_close();   // защита от двойного открытия
    memset(&s_pak, 0, sizeof s_pak);
    if (mz_zip_reader_init_file(&s_pak, path, 0)) {
        s_pak_open = 1;
        SDL_Log("Map pak opened: %s", path);
        return 1;
    }
    SDL_Log("Map pak open failed: %s", path);
    return 0;
}

void resources_pak_close(void)
{
    if (s_pak_open) {
        mz_zip_reader_end(&s_pak);
        s_pak_open = 0;
        cache_purge_pak();       // ресурсы карты — долой из кеша
        SDL_Log("Map pak closed");
    }
}

int resources_pak_is_open(void)
{
    return s_pak_open;
}

// Извлекает файл из пака (malloc'нутый буфер) или NULL
static unsigned char *pak_read(const char *path, unsigned int *size)
{
    if (!s_pak_open) return NULL;
    size_t n = 0;
    void *buf = mz_zip_reader_extract_file_to_heap(&s_pak, path, &n, 0);
    if (!buf) return NULL;
    if (size) *size = (unsigned int)n;
    return buf;
}

// --- Файлы: пак карты -> диск -> ../диск --------------------------------------
// Игра подсасывает ресурсы из своей папки (или на уровень выше — при
// запуске из build/). Вшитых данных нет: для запуска нужны папки рядом.
unsigned char *resources_read_file(const char *path, unsigned int *size)
{
    // Пак: miniz отдаёт ровно размер файла — добавляем завершающий \0
    // (нужен текстовым: map.json, шейдеры, скрипты).
    unsigned int psz = 0;
    unsigned char *pb = pak_read(path, &psz);
    if (pb) {
        unsigned char *buf = malloc((size_t)psz + 1);
        if (buf) {
            memcpy(buf, pb, psz);
            buf[psz] = 0;
        }
        free(pb);
        if (size) *size = psz;
        return buf;
    }

    char alt[512];
    snprintf(alt, sizeof alt, "../%s", path);
    const char *candidates[] = { path, alt };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long szl = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = malloc((size_t)szl + 1);
        if (buf && fread(buf, 1, (size_t)szl, f) == (size_t)szl) {
            fclose(f);
            buf[szl] = 0;                 // завершающий \0 — и для текста
            if (size) *size = (unsigned int)szl;
            return buf;
        }
        free(buf);
        fclose(f);
    }
    return NULL;
}

char *resources_read_text(const char *path)
{
    unsigned int sz = 0;
    return (char *)resources_read_file(path, &sz);
}

// --- Кеш: общий список записей -------------------------------------------------
typedef struct ResEntry {
    char key[256];       // ключ (путь#параметры)
    void *data;          // пиксели / GL-текстура / GL-программа
    int refs;            // счётчик ссылок (текстуры/программы)
    int from_pak;        // 1 = ресурс пришёл из zip карты (удаляется со сменой карты)
    int iw, ih;          // размеры (картинки)
    // Шейдерная программа: пути и mtime файлов (для hot-reload, см. resources_shader_reload)
    char vs[128], fs[128];
    long vs_mt, fs_mt;
    struct ResEntry *next;
} ResEntry;

static ResEntry *s_cache;

static ResEntry *cache_find(const char *key)
{
    for (ResEntry *e = s_cache; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

static ResEntry *cache_add(const char *key, void *data, int refs)
{
    ResEntry *e = malloc(sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key);
    e->data = data;
    e->refs = refs;
    e->from_pak = s_pak_open;   // ресурс из текущей zip-карты
    e->iw = e->ih = 0;
    e->next = s_cache;
    s_cache = e;
    return e;
}

// Удаляет запись из списка (свобождает структуру, data не трогает).
static void cache_remove(ResEntry *e)
{
    if (s_cache == e) {
        s_cache = e->next;
    } else {
        ResEntry *p;
        for (p = s_cache; p && p->next != e; p = p->next)
            ;
        if (p) p->next = e->next;
    }
    free(e);
}

// Удаляет из кеша все ресурсы, пришедшие из пака карты: при смене карты
// те же пути (img/tree.png и т.п.) обязаны загрузиться заново.
static void cache_purge_pak(void)
{
    ResEntry *e = s_cache;
    while (e) {
        ResEntry *next = e->next;
        if (e->from_pak) {
            if (strncmp(e->key, "tex#", 4) == 0) {
                GLuint t = (GLuint)(uintptr_t)e->data;
                glDeleteTextures(1, &t);
            } else if (strncmp(e->key, "prog#", 5) == 0) {
                glDeleteProgram((GLuint)(uintptr_t)e->data);
            } else {
                stbi_image_free(e->data);
            }
            cache_remove(e);
        }
        e = next;
    }
}

// --- Картинки ----------------------------------------------------------------
unsigned char *resources_image(const char *path, int *w, int *h, int channels)
{
    char key[256];
    snprintf(key, sizeof key, "img#%s#%d", path, channels);
    ResEntry *e = cache_find(key);
    if (e) {
        if (w) *w = e->iw;
        if (h) *h = e->ih;
        return e->data;
    }

    int iw = 0, ih = 0, c = 0;
    unsigned char *p = NULL;
    {   // пак карты -> диск -> ../диск
        unsigned int sz = 0;
        unsigned char *pb = pak_read(path, &sz);
        if (pb) {
            p = stbi_load_from_memory(pb, (int)sz, &iw, &ih, &c, channels);
            free(pb);
        }
        if (!p) {
            char alt[512];
            snprintf(alt, sizeof alt, "../%s", path);
            const char *candidates[] = { path, alt };
            for (int i = 0; i < 2 && !p; i++)
                p = stbi_load(candidates[i], &iw, &ih, &c, channels);
        }
    }
    if (!p) {
        SDL_Log("Cannot load %s: %s", path, stbi_failure_reason());
        return NULL;
    }
    ResEntry *ne = cache_add(key, p, 1);
    ne->iw = iw;
    ne->ih = ih;
    if (w) *w = iw;
    if (h) *h = ih;
    return p;
}

// --- Текстуры ----------------------------------------------------------------
GLuint resources_texture(const char *path, int channels, int wrap, int linear)
{
    char key[256];
    snprintf(key, sizeof key, "tex#%s#%d#%d#%d", path, channels, wrap, linear);
    ResEntry *e = cache_find(key);
    if (e) {
        e->refs++;
        return (GLuint)(uintptr_t)e->data;
    }
    int w = 0, h = 0;
    unsigned char *img = resources_image(path, &w, &h, channels);
    if (!img) return 0;
    GLuint tex = make_texture(w, h, img, channels == 4 ? GL_RGBA : GL_RGB,
                              wrap, linear);
    ResEntry *ne = cache_add(key, (void *)(uintptr_t)tex, 1);
    ne->iw = w;
    ne->ih = h;
    return tex;
}

void resources_texture_release(GLuint tex)
{
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "tex#", 4) != 0) continue;
        if ((GLuint)(uintptr_t)e->data != tex) continue;
        if (--e->refs <= 0) {
            glDeleteTextures(1, &tex);
            cache_remove(e);
        }
        return;
    }
    SDL_Log("resources: release of unknown texture %u", tex);
}

// --- Шейдерные программы ------------------------------------------------------
// Линкует vs+fs в программу (общая для создания и hot-reload'а)
static GLuint program_link(const char *vs_path, const char *fs_path)
{
    GLuint vs = compile_shader_file(GL_VERTEX_SHADER, vs_path);
    GLuint fs = compile_shader_file(GL_FRAGMENT_SHADER, fs_path);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        SDL_Log("Program link error (%s + %s): %s", vs_path, fs_path, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint resources_program(const char *vs_path, const char *fs_path)
{
    char key[256];
    snprintf(key, sizeof key, "prog#%s#%s", vs_path, fs_path);
    ResEntry *e = cache_find(key);
    if (e) {
        e->refs++;
        return (GLuint)(uintptr_t)e->data;
    }
    GLuint prog = program_link(vs_path, fs_path);
    if (!prog) return 0;
    ResEntry *ne = cache_add(key, (void *)(uintptr_t)prog, 1);
    // Для hot-reload: пути и mtime файлов. cache_add ставит from_pak = "пак
    // открыт", но шейдер может лежать на ДИСКЕ (пак содержит только свои
    // шейдеры карты). Уточняем по факту наличия файла в архиве.
    if (s_pak_open)
        ne->from_pak = (mz_zip_reader_locate_file(&s_pak, vs_path, NULL, 0) >= 0 ||
                        mz_zip_reader_locate_file(&s_pak, fs_path, NULL, 0) >= 0);
    else
        ne->from_pak = 0;
    snprintf(ne->vs, sizeof ne->vs, "%s", vs_path);
    snprintf(ne->fs, sizeof ne->fs, "%s", fs_path);
    if (!ne->from_pak) {
        struct stat st;
        if (stat(vs_path, &st) == 0) ne->vs_mt = (long)st.st_mtime;
        if (stat(fs_path, &st) == 0) ne->fs_mt = (long)st.st_mtime;
    }
    return prog;
}

// --- Hot-reload шейдеров --------------------------------------------------------
#define RESHADE_MAX 8
static ReshadeFn s_reshade[RESHADE_MAX];
static int s_reshade_n = 0;

void resources_on_reshade(ReshadeFn fn)
{
    if (!fn || s_reshade_n >= RESHADE_MAX) return;
    for (int i = 0; i < s_reshade_n; i++)
        if (s_reshade[i] == fn) return;
    s_reshade[s_reshade_n++] = fn;
}

int resources_shader_reload(void)
{
    int changed = 0;
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "prog#", 5) != 0) continue;
        if (e->from_pak || e->vs[0] == 0) continue;
        struct stat st;
        long mt0 = 0, mt1 = 0;
        if (stat(e->vs, &st) == 0) mt0 = (long)st.st_mtime;
        if (stat(e->fs, &st) == 0) mt1 = (long)st.st_mtime;
        if (mt0 == e->vs_mt && mt1 == e->fs_mt) continue;
        // Перекомпиляция: новый id встаёт в кеш, старую программу удаляем
        GLuint prog = program_link(e->vs, e->fs);
        if (!prog) continue;
        glDeleteProgram((GLuint)(uintptr_t)e->data);
        e->data = (void *)(uintptr_t)prog;
        e->vs_mt = mt0;
        e->fs_mt = mt1;
        SDL_Log("shader reloaded: %s + %s", e->vs, e->fs);
        changed++;
    }
    if (changed)
        for (int i = 0; i < s_reshade_n; i++)
            s_reshade[i]();
    return changed;
}

void resources_program_release(GLuint prog)
{
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "prog#", 5) != 0) continue;
        if ((GLuint)(uintptr_t)e->data != prog) continue;
        if (--e->refs <= 0) {
            glDeleteProgram(prog);
            cache_remove(e);
        }
        return;
    }
    SDL_Log("resources: release of unknown program %u", prog);
}

// --- Жизненный цикл -----------------------------------------------------------
void resources_init(void)
{
    s_cache = NULL;
}

void resources_shutdown(void)
{
    int n = 0;
    ResEntry *e = s_cache;
    while (e) {
        ResEntry *next = e->next;
        if (strncmp(e->key, "tex#", 4) == 0) {
            GLuint t = (GLuint)(uintptr_t)e->data;
            glDeleteTextures(1, &t);
        } else if (strncmp(e->key, "prog#", 5) == 0) {
            glDeleteProgram((GLuint)(uintptr_t)e->data);
        } else {
            stbi_image_free(e->data);
        }
        free(e);
        e = next;
        n++;
    }
    s_cache = NULL;
    SDL_Log("resources: %d cached entries freed", n);
}
