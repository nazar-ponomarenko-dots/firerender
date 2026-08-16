// ============================================================================
//  resources.h — единый менеджер ресурсов: файлы, картинки, текстуры, шейдеры
// ----------------------------------------------------------------------------
//  Вся загрузка в проекте идёт через ОДИН модуль:
//    - файлы:    пак карты -> диск -> ../диск (в папке игры или выше);
//    - картинки: stb_image с кешем (ключ = путь + число каналов);
//    - текстуры: GL-текстуры с кешем и подсчётом ссылок — один файл
//                (напр. img/tree.png у 60 деревьев) грузится в GL один раз;
//    - шейдеры:  кеш по паре (vs, fs) — один и тот же шейдер компилируется
//                один раз (например field.vs+field.fs у FieldArea и Selection).
//  Картинки живут до resources_shutdown() и вручную НЕ освобождаются;
//  текстуры/программы освобождаются по счётчику ссылок
//  (resources_*_release вместо glDelete*).
// ============================================================================
#ifndef RESOURCES_H
#define RESOURCES_H
#include <stddef.h>       // size_t
#include "gl_loader.h"

// Читает файл целиком: ПАК КАРТЫ (если открыт) -> диск -> ../диск -> вшитые данные.
// Возвращает malloc'нутый буфер (освобождать free()) или NULL.
unsigned char *resources_read_file(const char *path, unsigned int *size);

// Текстовый файл (то же, что read_file, но с завершающим \0).
char *resources_read_text(const char *path);

// Загружает картинку через stb_image (с кешем). Возвращает указатель на
// данные, которыми ВЛАДЕЕТ кеш — НЕ освобождать; живут до shutdown().
// channels: 3 = RGB, 4 = RGBA. Если файла нет — подставляет img/notfound.png
// (Source Engine style). NULL — только если и notfound.png отсутствует.
unsigned char *resources_image(const char *path, int *w, int *h, int channels);

// Строгий вариант без подмены: NULL — если файла нет. Для данных
// (карта высот), которые не являются рендер-текстурами.
unsigned char *resources_image_strict(const char *path, int *w, int *h,
                                      int channels);

// Текстура из файла с кешем и подсчётом ссылок.
// wrap: GL_REPEAT / GL_CLAMP_TO_EDGE; linear: 1 = GL_LINEAR, 0 = GL_NEAREST.
GLuint resources_texture(const char *path, int channels, int wrap, int linear);

// Шейдерная программа из файлов (кеш + подсчёт ссылок).
GLuint resources_program(const char *vs_path, const char *fs_path);

// Уменьшить счётчик ссылок ресурса; при нуле — удалить GL-объект.
// Использовать ВМЕСТО glDeleteProgram/glDeleteTextures для ресурсов кеша.
void resources_program_release(GLuint prog);
void resources_texture_release(GLuint tex);

// Перезапрос программы при hot-reload: берёт свежую программу пары и отдаёт
// ссылку на старую. old == 0 (первый запрос из *_init) — просто загрузка.
// Если компиляция не удалась, возвращает 0 и НЕ трогает старый id — вызывающий
// обязан оставить узлу прежнюю программу (она жива до релиза).
GLuint resources_program_replace(GLuint old, const char *vs_path,
                                 const char *fs_path);

// Инициализация — до любых загрузок (в начале main), и полная очистка —
// в самом конце main, ПОСЛЕ освобождения всех модулей сцены.
void resources_init(void);
void resources_shutdown(void);

// --- Hot-reload шейдеров (см. shader.h) ---------------------------------------
// Хук пересборки: вызывается после перекомпиляции изменённых программ
// (модуль повторно ищет uniform'ы и статичные uniform'ы). До 8 хуков.
typedef void (*ReshadeFn)(void);
void resources_on_reshade(ReshadeFn fn);

// Перекомпилирует в кеше шейдерные программы, чьи файлы изменились (по mtime).
// Программы из zip-пака карты не проверяются. Возвращает число перекомпили-
// рованных; при изменении зовёт все зарегистрированные хуки.
int resources_shader_reload(void);

// Hot-reload текстур: проверяет mtime файлов текстур на диске и
// перезагружает изменённые. Текстуры из zip-пака не проверяются.
// Возвращает число перезагруженных текстур.
int resources_texture_reload(void);

// --- Пак карты (maps/*.zip) ---------------------------------------------------
// Открывает zip-архив карты: его файлы получают приоритет над диском и
// вшитыми данными (карта может подсосать движковые ресурсы — их просто
// кладут в архив либо оставляют в img/ движка). Вызывать при загрузке карты.
// Возвращает 1, если архив открылся.
int resources_pak_open(const char *path);
void resources_pak_close(void);
int resources_pak_is_open(void);

// Устанавливает базовую директорию ресурсов (путь к папке с map/, shaders/, img/).
// Вызывать один раз при старте ДО любых загрузок ресурсов.
void resources_set_base_dir(const char *dir);

// Резолвит относительный путь в абсолютный, используя базовую директорию
// и fallback-цепочку: base_dir/path, base_dir/../path, path, ../path.
// Возвращает 1 если файл найден, 0 если нет. out буфер должен быть >= 512 байт.
int resources_resolve_path(const char *rel_path, char *out, size_t out_sz);

#endif
