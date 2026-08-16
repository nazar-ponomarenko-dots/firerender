// ============================================================================
//  cvars.h — переменные движка (CVar'ы) и config.cfg
// ----------------------------------------------------------------------------
//  Реестр переменных, которыми управляют консоль и клавиши: fov, cl_sens,
//  sv_gravity, r_shadows... Модули ЧИТАЮТ cvar'ы каждый кадр, консоль их
//  меняет — связь односторонняя, без прямых вызовов.
//
//  Переменная: имя, значение, диапазон [min, max] (set зажимает), флаги.
//  config.cfg — файл "name value" в рабочей папке: грузится при старте,
//  сохраняется при выходе (как в GoldSrc). Сохраняются только cvar'ы
//  с флагом CVAR_ARCHIVE.
// ============================================================================
#ifndef CVARS_H
#define CVARS_H

typedef struct {
    char name[32];       // имя переменной
    float value;         // текущее значение
    float def;           // значение по умолчанию
    float minv, maxv;    // диапазон (minv > maxv — без ограничений)
    int flags;           // CVAR_ARCHIVE и др.
    char str[64];        // строковое значение (для string-переменных)
    char def_str[64];    // дефолтное строковое значение
    char help[64];       // описание (для cvarlist)
} Cvar;

#define CVARS_MAX 64

// Флаги переменных
#define CVAR_ARCHIVE   1  // сохранять в config.cfg (по умолчанию для register)
#define CVAR_NOSAVE    2  // НЕ сохранять (временные/служебные)
#define CVAR_CHEAT     4  // чит: меняется только при sv_cheats 1
#define CVAR_USERINFO  8  // инфо игрока: шлётся/сохраняется, но не важна для
                          // игрового мира (как FCVAR_USERINFO в Source)
#define CVAR_STRING   16  // строковая переменная (значение в c->str)

// Очищает реестр (вызывается console_init).
void cvar_init(void);

// Регистрирует переменную: без диапазона, с флагом CVAR_ARCHIVE.
void cvar_register(const char *name, float def, const char *help);

// Регистрирует с диапазоном [min, max] и флагами (см. CVAR_*).
// min >= max — без ограничений. Повторная регистрация имени — no-op.
void cvar_register_ex(const char *name, float def, float min, float max,
                      int flags, const char *help);

// Регистрирует СТРОКОВУЮ переменную (флаг CVAR_STRING ставится сам).
// Значение по умолчанию — строка; cvar_get(name) вернёт atof(def_str).
// Повторная регистрация имени — no-op.
void cvar_register_string(const char *name, const char *def, const char *help);

// Чтение/запись. cvar_get незнакомого имени = 0; cvar_set незнакомого
// имени регистрирует его (консоль может заводить свои переменные).
// cvar_set зажимает значение в диапазон. Для string-переменных cvar_set
// пишет и строковое значение.
float cvar_get(const char *name);
void cvar_set(const char *name, float v);

// Строковые переменные (CVAR_STRING). cvar_get_string возвращает
// "" для нестроковой/неизвестной переменной.
const char *cvar_get_string(const char *name);
void cvar_set_string(const char *name, const char *s);

// Устанавливает значение из СТРОКИ: если переменная CVAR_STRING — пишет
// строку, иначе — atof() (используется при загрузке config.cfg и set).
void cvar_set_from_string(const char *name, const char *s);

// Доступ по индексу (для cvarlist и Tab-дополнения).
int cvar_index(const char *name);   // -1, если нет
int cvar_count(void);
const Cvar *cvar_at(int i);

// config.cfg: строки "name value". load возвращает 1, если файл открылся.
// save пишет только cvar'ы с CVAR_ARCHIVE.
int cvar_load_config(const char *path);
void cvar_save_config(const char *path);

#endif
