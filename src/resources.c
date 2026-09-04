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
#include <unistd.h>   // access()
#include <time.h>

// --- Пак карты (maps/*.zip) ----------------------------------------------------
static mz_zip_archive s_pak;
static int s_pak_open = 0;
static char s_base_dir[512] = ".";   // базовая директория ресурсов (по умолчанию CWD)

// Устанавливает базовую директорию для поиска ресурсов.
// Вызывать один раз при старте (из main, после определения пути к exe).
void resources_set_base_dir(const char *dir)
{
    if (!dir || !dir[0]) return;
    snprintf(s_base_dir, sizeof s_base_dir, "%s", dir);
    // Убираем trailing slash
    size_t len = strlen(s_base_dir);
    if (len > 1 && (s_base_dir[len-1] == '/' || s_base_dir[len-1] == '\\'))
        s_base_dir[len-1] = 0;
    SDL_Log("Resources base dir: %s", s_base_dir);
}

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
// запуске с build/). Вшитых данных нет: для запуска нужны папки рядом.
// Резолвит относительный путь в первый существующий файл по цепочке fallback
// (единый источник правды для ВСЕХ модулей: audio, mapdata, lua, textures).
int resources_resolve_path(const char *rel_path, char *out, size_t out_sz)
{
    char path[512];
    snprintf(path, sizeof path, "%s", rel_path);
    // Локальный файл (относительный путь уже абсолютный или есть в CWD)
    if (access(path, F_OK) == 0) {
        snprintf(out, out_sz, "%s", path);
        return 1;
    }
    // Через базовую директорию
    if (s_base_dir[0] && s_base_dir[0] != '.') {
        char cand[512];
        snprintf(cand, sizeof cand, "%s/%s", s_base_dir, path);
        if (access(cand, F_OK) == 0) {
            snprintf(out, out_sz, "%s", cand);
            return 1;
        }
        snprintf(cand, sizeof cand, "%s/../%s", s_base_dir, path);
        if (access(cand, F_OK) == 0) {
            snprintf(out, out_sz, "%s", cand);
            return 1;
        }
    }
    // Родительская директория текущей
    char up[512];
    snprintf(up, sizeof up, "../%s", path);
    if (access(up, F_OK) == 0) {
        snprintf(out, out_sz, "%s", up);
        return 1;
    }
    return 0;
}

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

    char resolved[512];
    if (resources_resolve_path(path, resolved, sizeof resolved)) {
        FILE *f = fopen(resolved, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long szl = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char *buf = malloc((size_t)szl + 1);
            if (buf && fread(buf, 1, (size_t)szl, f) == (size_t)szl) {
                fclose(f);
                buf[szl] = 0;
                if (size) *size = (unsigned int)szl;
                return buf;
            }
            free(buf);
            fclose(f);
        }
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
    // Текстура: параметры создания и mtime файла (для hot-reload текстур)
    char tex_path[128];
    int tex_channels, tex_wrap, tex_linear;
    long tex_mt;
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
// Использует безопасную итерацию через указатель-на-указатель, чтобы
// корректно удалять несколько подряд идущих элементов.
static void cache_purge_pak(void)
{
    ResEntry **pp = &s_cache;
    while (*pp) {
        ResEntry *e = *pp;
        if (e->from_pak) {
            // Пак карты закрывается — его GL-объекты больше недействительны.
            // Обычно scene_teardown до этого освободил их через refcount
            // (resources_*_release: refs->0 + glDelete + cache_remove), и в
            // кеше их уже нет. Если запись ещё здесь (refs > 0 — кто-то не
            // дал release), освобождаем объект принудительно, иначе он
            // утекает навсегда. img# (stbi-пиксели) принадлежат кешу.
            if (strncmp(e->key, "tex#", 4) == 0) {
                GLuint t = (GLuint)(uintptr_t)e->data;
                glDeleteTextures(1, &t);
            } else if (strncmp(e->key, "prog#", 5) == 0) {
                glDeleteProgram((GLuint)(uintptr_t)e->data);
            } else if (strncmp(e->key, "img#", 4) == 0) {
                stbi_image_free(e->data);
            }
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

// mtime файла через единый резолвер путей (как resources_read_file)
static long file_mtime(const char *path)
{
    char resolved[512];
    struct stat st;
    if (resources_resolve_path(path, resolved, sizeof resolved) &&
        stat(resolved, &st) == 0)
        return (long)st.st_mtime;
    return 0;
}

// --- Картинки ----------------------------------------------------------------
// Загружает пиксели БЕЗ кеша (пак карты -> диск через резолвер).
// Возвращает malloc-буфер (stbi), который обязан освободить вызывающий
// через stbi_image_free(). NULL — не загрузилась.
// from_pak (может быть NULL) получает 1, если данные ВЗЯЛИСЬ ИЗ ПАКА карты:
// только такие записи кеша обязаны умереть вместе с паком (cache_purge_pak).
// Движковые img/*.png читаются с диска и живут между картами.
static unsigned char *image_load_raw(const char *path, int *w, int *h,
                                     int channels, int *from_pak)
{
    int iw = 0, ih = 0, c = 0;
    unsigned char *p = NULL;
    if (from_pak) *from_pak = 0;
    {   // пак карты
        unsigned int sz = 0;
        unsigned char *pb = pak_read(path, &sz);
        if (pb) {
            p = stbi_load_from_memory(pb, (int)sz, &iw, &ih, &c, channels);
            free(pb);
            if (p && from_pak) *from_pak = 1;
        }
        if (!p) {
            char resolved[512];
            if (resources_resolve_path(path, resolved, sizeof resolved))
                p = stbi_load(resolved, &iw, &ih, &c, channels);
        }
    }
    if (!p) {
        SDL_Log("Cannot load %s: %s", path, stbi_failure_reason());
        return NULL;
    }
    if (w) *w = iw;
    if (h) *h = ih;
    return p;
}

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

    int from_pak = 0;
    unsigned char *p = image_load_raw(path, w, h, channels, &from_pak);
    if (!p) {
        // Source Engine style: отсутствующая картинка заменяется notfound.png.
        // Если и он пропал — честный NULL (не рекурсия: ниже защита).
        if (strcmp(path, "img/notfound.png") == 0) return NULL;
        SDL_Log("Texture %s not found -> using img/notfound.png", path);
        p = image_load_raw("img/notfound.png", w, h, channels, &from_pak);
    }
    if (!p) return NULL;
    ResEntry *ne = cache_add(key, p, 1);
    ne->from_pak = from_pak;      // из пака — только если реально из архива
    ne->iw = *w;
    ne->ih = *h;
    return p;
}

// Строгий вариант без подмены не-текстурных данных (карта высот): NULL —
// если картинки нет вообще, не подставлять notfound.png.
unsigned char *resources_image_strict(const char *path, int *w, int *h,
                                      int channels)
{
    return image_load_raw(path, w, h, channels, NULL);
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
    if (!img) {
        // Source Engine style: отсутствующая текстура заменяется notfound.png.
        // Если и он пропал — честный 0 (чёрный) вместо мусора (без рекурсии).
        if (strcmp(path, "img/notfound.png") == 0) return 0;
        return resources_texture("img/notfound.png", channels, wrap, linear);
    }
    GLuint tex = make_texture(w, h, img, channels == 4 ? GL_RGBA : GL_RGB,
                              wrap, linear);
    ResEntry *ne = cache_add(key, (void *)(uintptr_t)tex, 1);
    // «Из пака» наследуем от записи ПИКСЕЛЕЙ: движковые img/*.png читаются с
    // диска даже при открытом паке и обязаны выживать смену карты (иначе на
    // повторной загрузке они грузятся заново — с чужими глобальными флагами
    // stb_image и лишней работой).
    {
        char ikey[256];
        snprintf(ikey, sizeof ikey, "img#%s#%d", path, channels);
        ResEntry *im = cache_find(ikey);
        ne->from_pak = im ? im->from_pak : 0;
    }
    ne->iw = w;
    ne->ih = h;
    // Для hot-reload текстур: путь и параметры + mtime (если не из пака)
    snprintf(ne->tex_path, sizeof ne->tex_path, "%s", path);
    ne->tex_channels = channels;
    ne->tex_wrap = wrap;
    ne->tex_linear = linear;
    ne->tex_mt = ne->from_pak ? 0 : file_mtime(path);
    return tex;
}

// Hot-reload текстур: перезагружает изменённые на диске файлы
int resources_texture_reload(void)
{
    int changed = 0;
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "tex#", 4) != 0) continue;
        if (e->from_pak || e->tex_path[0] == 0) continue;
        long mt = file_mtime(e->tex_path);
        if (mt == e->tex_mt) continue;
        int w = 0, h = 0;
        unsigned char *img = image_load_raw(e->tex_path, &w, &h,
                                            e->tex_channels, NULL);
        if (!img) continue;
        GLuint tex = (GLuint)(uintptr_t)e->data;
        GLenum fmt = e->tex_channels == 4 ? GL_RGBA : GL_RGB;
        GLint internal = fmt == GL_RGBA ? GL_RGBA8
                       : fmt == GL_RED ? GL_R8
                       : GL_RGB8;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        e->tex_linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        e->tex_linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, e->tex_wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, e->tex_wrap);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt,
                     GL_UNSIGNED_BYTE, img);
        e->iw = w;
        e->ih = h;
        e->tex_mt = mt;
        // Обновляем и кеш картинки (img#...) — свежие пиксели для новых текстур
        {
            char ikey[256];
            snprintf(ikey, sizeof ikey, "img#%s#%d", e->tex_path,
                     e->tex_channels);
            ResEntry *im = cache_find(ikey);
            if (im) {
                stbi_image_free(im->data);
                im->data = img;
                im->iw = w;
                im->ih = h;
            } else {
                stbi_image_free(img);
            }
        }
        SDL_Log("texture reloaded: %s", e->tex_path);
        changed++;
    }
    return changed;
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
        ne->vs_mt = file_mtime(vs_path);
        ne->fs_mt = file_mtime(fs_path);
    }
    return prog;
}

// --- Hot-reload шейдеров --------------------------------------------------------
#define RESHADE_MAX 8
static ReshadeFn s_reshade[RESHADE_MAX];
static int s_reshade_n = 0;

// Отложенное удаление старых программ после hot-reload. Старую программу НЕЛЬЗЯ
// удалять сразу: её id ещё держат живые узлы (glb/mesh/anim не слушают reshade),
// и они продолжают её рисовать. Держим до тех пор, пока ВСЕ держатели не вернут
// её через resources_program_release (remaining — сколько релизов осталось).
typedef struct { GLuint prog; int remaining; } PendingProg;
#define PROG_PENDING_MAX 128
static PendingProg s_prog_pending[PROG_PENDING_MAX];
static int s_prog_pending_n = 0;

static void prog_pending_add(GLuint prog, int remaining)
{
    if (remaining <= 0) { glDeleteProgram(prog); return; }   // держателей нет
    // Уже есть такой id (повторный reload до релиза) — складываем релизы
    for (int i = 0; i < s_prog_pending_n; i++)
        if (s_prog_pending[i].prog == prog) { s_prog_pending[i].remaining += remaining; return; }
    if (s_prog_pending_n >= PROG_PENDING_MAX) { glDeleteProgram(prog); return; } // предохранитель
    s_prog_pending[s_prog_pending_n].prog = prog;
    s_prog_pending[s_prog_pending_n].remaining = remaining;
    s_prog_pending_n++;
}

// Возвращает 1, если id найден в отложенных и обработан (релиз засчитан).
static int prog_pending_release(GLuint prog)
{
    for (int i = 0; i < s_prog_pending_n; i++) {
        if (s_prog_pending[i].prog != prog) continue;
        if (--s_prog_pending[i].remaining <= 0) {
            glDeleteProgram(prog);
            s_prog_pending[i] = s_prog_pending[--s_prog_pending_n];
        }
        return 1;
    }
    return 0;
}

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
        long mt0 = file_mtime(e->vs);
        long mt1 = file_mtime(e->fs);
        if (mt0 == e->vs_mt && mt1 == e->fs_mt) continue;
        // Перекомпиляция: новый id встаёт в кеш, а старую программу отдаём в
        // отложенное удаление — её ещё рисуют узлы, не слушающие reshade.
        // ВАЖНО: refcount счёта СТАРОЙ программы уходит в pending, поэтому для
        // нового id счётчик обнуляем: узлы, слушающие reshade, пере-запрашивают
        // его через make_program (+1 ref), а старые держатели релизят свой
        // старый id через отложенный список. Не обнулим — новый id унаследует
        // чужой счёт и никогда не удалится.
        GLuint prog = program_link(e->vs, e->fs);
        if (!prog) continue;
        GLuint old = (GLuint)(uintptr_t)e->data;
        prog_pending_add(old, e->refs);
        e->data = (void *)(uintptr_t)prog;
        e->refs = 0;                 // новый id: держатели пере-запросят
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
    if (!prog) return;
    // Сначала — отложенные (старые после hot-reload) программы
    if (prog_pending_release(prog)) return;
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

// Перезапрос программы для hot-reload: берёт свежую ссылку на текущую
// программу пары и только потом отдаёт старую. Именно это балансирует
// refcount при reload: без release(old) каждый hot-reload утекал по одной
// ссылке на программу у каждого слушателя reshade.
// Порядок важен: сперва запрашиваем новую (если пара не собралась — старый
// id остаётся у узла жив и невредим), релиз старой — после успеха.
// old == 0 (первый вызов из *_init) — release пропускается.
GLuint resources_program_replace(GLuint old, const char *vs_path,
                                 const char *fs_path)
{
    GLuint prog = resources_program(vs_path, fs_path);
    if (!prog) return 0;                 // не собралась — старую не трогаем
    if (old) resources_program_release(old);
    return prog;
}

// --- Жизненный цикл -----------------------------------------------------------
void resources_init(void)
{
    s_cache = NULL;
}

void resources_shutdown(void)
{
    // Отложенные программы (старые id после hot-reload): их держатели могли
    // не дожить до shutdown — на выходе удаляем всё разом.
    for (int i = 0; i < s_prog_pending_n; i++)
        glDeleteProgram(s_prog_pending[i].prog);
    s_prog_pending_n = 0;
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
