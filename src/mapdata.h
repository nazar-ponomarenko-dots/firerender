// ============================================================================
//  mapdata.h — данные карты из map/map.json: поле, источники света, объекты
// ----------------------------------------------------------------------------
//  Всё, что касается карты, лежит в папке map/ и подтягивается игрой оттуда:
//    map/map.png   — карта высот (яркость = высота, цвет = цвет земли);
//    map/map.json  — метаданные: путь к карте, прямоугольник поля,
//                    список источников света, список объектов (моделей).
//  JSON парсится мини-парсером json.h один раз при старте (mapdata_load),
//  дальше модули сцены читают значения через функции ниже. Если какого-то
//  блока в JSON нет — модули используют свои значения по умолчанию.
// ============================================================================
#ifndef MAPDATA_H
#define MAPDATA_H
#include "material.h"

// Типы источников света (для будущих точечных/прожекторов)
typedef enum {
    LIGHT_DIRECTIONAL,   // направленный (солнце)
    LIGHT_POINT,         // точечный
    LIGHT_SPOT           // прожектор
} MapLightType;

typedef struct {
    MapLightType type;
    char name[64];       // имя источника (для поиска из кода)
    float pos[3];        // позиция (point/spot)
    float dir[3];        // направление (directional/spot)
    float color[3];      // цвет света
    float intensity;     // сила света
    float range;         // радиус действия (point/spot), м
    float angle;         // полуугол конуса (spot), рад
    int shadow_on;       // свет отбрасывает тени? (shadow/enabled)
    float shadow_bias;   // bias теней рельефа (shadow/bias, латеральных текселей)
    float shadow_bias_veg; // bias теней растительности (shadow/bias_veg)
    int attached;        // 1 = следует за игроком каждый кадр (point-свет)
} MapLight;

// Материал объекта — единый тип (см. material.h): blend/cull/wrap/filter/
// tint/alpha/UV-анимация. Парсится из блока "render" в map.json.
// --- Коллизии карты: collisions[] — произвольные квадраты ----------------------
typedef struct {
    char object[64];     // имя объекта, к которому привязан ("tree1") или ""
    float pos[3];        // центр (x, y, z) или смещение от объекта
    float size[2];       // [w, d] — размер квадрата в метрах
    float height;        // высота коллизии, м (задел; сейчас блок по XZ)
} MapCollision;

// --- Позиционные звуки карты: sounds[] -----------------------------------------
typedef struct {
    float pos[3];        // позиция источника, м
    char sound[256];     // путь к wav
    float volume;        // громкость 0..1
    float radius;        // радиус слышимости, м
} MapSound3D;

// --- Команды/сообщение/звук — общие для use и triggers -------------------------
#define MAP_ACTIONS_MAX  8     // максимум команд в действии
#define MAP_CMD_LEN      256

// Блок "use" объекта: что происходит при нажатии E рядом
typedef struct {
    int set;
    char commands[MAP_ACTIONS_MAX][MAP_CMD_LEN];
    int cmd_n;
    char message[256];
    char sound[256];
    int once;            // сработать один раз
    int used;
} MapUse;

// Триггер: прямоугольная область, срабатывает при входе игрока
typedef struct {
    char name[64];
    float pos[3];        // центр области
    float size[2];       // [w, d] в метрах
    int once;            // сработать один раз
    int entered;         // игрок был внутри в прошлом кадре (движок)
    int fired;           // уже сработал (движок)
    char commands[MAP_ACTIONS_MAX][MAP_CMD_LEN];
    int cmd_n;
    char message[256];
    char sound[256];
    float teleport[3];   // телепорт при входе (если has_tp)
    int has_tp;
} MapTrigger;

// --- Объекты: objects[] --------------------------------------------------------
// Встроенные шейдеры объекта (MapObject.shader_kind)
typedef enum {
    OBJ_SHADER_DEFAULT,   // крест из двух квадов, полное освещение
    OBJ_SHADER_BILLBOARD, // один квад всегда лицом к камере, полное освещение
    OBJ_SHADER_FLAT,      // без освещения: текстура * tint (иконки, UI)
    OBJ_SHADER_CUSTOM     // свой шейдер (vs/fs из карты)
} MapObjectShader;

typedef struct {
    char name[64];       // имя объекта
    char model[256];     // путь к текстуре-спрайту (PNG, относительно карты)
    char class[64];      // класс сущности (entity-система, по умолчанию "prop_static")
    float pos[3];        // позиция
    float rot[3];        // поворот (радианы)
    float scale[3];      // масштаб
    int billboard;       // 1 — квад всегда лицом к камере (вместо креста)
    int collision;       // 1 — объект непроходим (цилиндр radius вокруг базы)
    float radius;        // радиус коллизии, м (по умолчанию 1.5)
    MapObjectShader shader_kind; // встроенный шейдер
    char shader_vs[256]; // свой вершинный шейдер (если custom)
    char shader_fs[256]; // свой фрагментный шейдер (если custom)
    Material render;         // материал (render = { ... }, см. material.h)
    MapUse use;              // взаимодействие по E (use = { ... })
} MapObject;

// --- Воксельный объёмный туман: fog = { ... } --------------------------------
typedef struct {
    int enabled;              // 1 — туман включён (клавиша P)
    float strength;           // общий множитель плотности
    float density_ground;     // дымка у земли (на метр пути)
    float ground_height;      // высота дымки у земли, м
    float density_field;      // доп. туман над полем пшеницы
    float field_height;       // высота полевого тумана, м
    float density_water;      // туман над водоёмами (на метр пути)
    float water_height;       // высота тумана над уровнем воды, м
    float color[3];           // БАЗОВЫЙ цвет тумана (везде)
    float color_sun[3];       // цвет солнечного рассеяния (тёплый, у солнца)
    float glare;              // ореол у самого солнца
    float ray_strength;       // контраст god rays (0 = нет, 2 = очень резкие)
} MapFog;

// --- Рельеф: terrain = { ... } ------------------------------------------------
typedef struct {
    float min_height, max_height;   // кривая высот: мин/макс мира
    float band_t0, band_t1;         // полоса игровых высот (по яркости)
    float band_h0, band_h1;         // высоты полосы
    int blur;                       // сглаживание карты высот (проходы)
    int blur_radius;                // радиус сглаживания, метры
    int erosion;                    // проходы ограничения крутизны (0 = выкл)
    float erosion_slope;            // макс. крутизна после эрозии, градусы
    char ground_tex[256];           // текстура земли (img/ground.png)
    float tex_scale;                // метр на тайл текстуры
    float field_color[3];           // тон почвы поля пшеницы
    float field_blend;              // сила смешивания (0..1)
    float fog_start, fog_end;       // дистанция тумана (множители мира)
} MapTerrain;

// --- Растительность: veg = { ... } -------------------------------------------
typedef struct {
    float grass_density, wheat_density;  // шт/м2 вплотную к камере
    float grass_floor, wheat_floor;      // минимум на всей карте
    float dandelion_density;             // одуванчики шт/м2 у камеры (редкие,
                                         // добавляются СВЕРХ травы)
    float dandelion_floor;               // минимум одуванчиков на карте
    float dyn_radius, vis_radius;        // радиусы динамики/видимости
    float falloff;                       // радиус падения плотности
} MapVeg;

// --- Небо: sky = { ... } ------------------------------------------------------
typedef struct {
    float tint[3];              // тон панорамы
    char tex[256];              // текстура неба (img/skybox.png)
} MapSky;

// --- Постобработка: bloom = { ... } -------------------------------------------
typedef struct {
    float threshold, intensity, strength;   // порог яркости, сила, свечение
} MapBloom;

// --- Тени: shadows = { ... } --------------------------------------------------
typedef struct {
    int size;                   // размер карт теней (1024/2048/4096)
} MapShadows;

// --- Информация о карте: info = { name, author, description } -----------------
typedef struct {
    char name[128];             // название (для меню/логов)
    char author[128];           // автор
    char description[256];      // описание
} MapInfo;

// --- Звук карты: ambient = { sound, volume }, music = { sound, volume } -------
typedef struct {
    char sound[256];            // путь к wav (относительно корня: audio/x.wav)
    float volume;               // громкость 0..1
} MapSound;

// Загружает map/map.json: путь к карте высот, прямоугольник поля,
// источники света и объекты. Вызывать ДО world_load (нужно имя карты).
// 0 — успех, -1 — файл не прочитался или JSON битый.
int mapdata_load(const char *path);

// Имя карты высот относительно map/ (по умолчанию "map.png")
const char *mapdata_image(void);

// Прямоугольник поля пшеницы в метрах: center[2] = (x, z), size[2] = (w, d).
// Возвращает 1, если поле задано в JSON, иначе 0 (брать центр мира).
int mapdata_field(float center[2], float size[2]);

// Поле, прикреплённое к игроку (field/attached): 1 — пшеница растёт на
// всей карте (бесконечное поле), рамка следует за камерой.
int mapdata_field_attached(void);

// Стартовая позиция игрока (player/pos [x,y,z]) и взгляд
// (player/yaw, player/pitch). Возвращает 1, если player задан в JSON.
// y <= 0 означает "высоту взять из рельефа в точке спавна".
// fov — угол обзора (player.fov, по умолчанию 60).
int mapdata_player(float pos[3], float *yaw, float *pitch, float *fov);

// --- Источники света ----------------------------------------------------------
int mapdata_light_count(void);
const MapLight *mapdata_light(int i);

// --- Воксельный объёмный туман (voxfog.c) -------------------------------------
void mapdata_fog(MapFog *f);

// --- Рельеф / растительность / небо / bloom / тени ---------------------------
void mapdata_terrain(MapTerrain *t);
void mapdata_veg(MapVeg *v);
void mapdata_sky(MapSky *s);
void mapdata_bloom(MapBloom *b);
void mapdata_shadows(MapShadows *s);

// --- Объекты (модели) ---------------------------------------------------------
int mapdata_object_count(void);
const MapObject *mapdata_object(int i);

// --- Коллизии-квадраты: collisions[] ------------------------------------------
int mapdata_collision_count(void);
const MapCollision *mapdata_collision(int i);

// --- Позиционные звуки: sounds[] -----------------------------------------------
#define MAP_MAX_SOUNDS 16
int mapdata_sound_count(void);
const MapSound3D *mapdata_sound(int i);

// --- Триггеры: triggers[] -------------------------------------------------------
#define MAP_MAX_TRIGGERS 64
int mapdata_trigger_count(void);
const MapTrigger *mapdata_trigger(int i);
int mapdata_trigger_fired(int i);
void mapdata_trigger_set_fired(int i, int v);
void mapdata_object_set_used(const char *name, int v);

// --- Вода: waters[] -----------------------------------------------------------
// Четыре типа водоёмов. Каждый блок задаёт уровень воды (level), форму
// (круг pos/radius для лужи/озера/моря или осевая линия points + width для
// реки) и вид (цвет, прозрачность, волны, течение, пена у берега).
typedef enum {
    WATER_PUDDLE,   // лужа: маленькая, почти гладкая, быстро сходит
    WATER_LAKE,     // озеро: спокойное, мягкая волна
    WATER_RIVER,    // река: течение вдоль осевой линии (flow_speed)
    WATER_SEA       // море: глобальный уровень воды, крупная волна
} MapWaterType;

#define MAP_MAX_WATERS        32   // потолок водоёмов из JSON
#define MAP_WATER_MAX_POINTS  16   // максимум точек осевой линии реки

typedef struct {
    int type;              // MapWaterType
    char name[64];         // имя водоёма (для поиска из кода)
    float level;           // уровень поверхности (метры); у моря — глобальный
    float center[2];       // центр круга [x, z] (лужа/озеро/море)
    float radius;          // радиус круга, м (море: 0 = весь мир)
    float width;           // ширина русла, м (река)
    float points[MAP_WATER_MAX_POINTS][2]; // осевая линия реки [x, z]
    int point_n;
    float depth;           // глубина: просвет дна сквозь воду, м
    float color[3];        // цвет воды (тон на глубине)
    float alpha;           // базовая непрозрачность 0..1
    float wave_amp;        // амплитуда волны, м
    float wave_speed;      // скорость волны (множитель времени)
    float flow[2];         // направление течения (река: авто из points по XZ)
    float flow_speed;      // скорость течения, м/с (река сносит игрока)
    float foam;            // сила пены у берега 0..1
} MapWater;

// --- Фауна: fauna[] -------------------------------------------------------------
typedef enum {
    FAUNA_FIREFLIES,   // светлячки (светящиеся точки, мерцают)
    FAUNA_BUTTERFLIES, // бабочки (машут крыльями, летают в области)
    FAUNA_BIRDS        // птицы (кружат в небе над областью)
} MapFaunaType;

typedef struct {
    int type;            // MapFaunaType
    float area[2];       // центр области [x, z]
    float size[2];       // [w, d]
    int count;           // число особей
    float height;        // высота полёта, м
} MapFauna;

#define MAP_MAX_FAUNA 8
int mapdata_fauna_count(void);
const MapFauna *mapdata_fauna(int i);

// --- Информация о карте -------------------------------------------------------
void mapdata_info(MapInfo *i);

// --- Звук карты: ambient/music. 1 — задан в JSON (играть петлёй). --------------
int mapdata_ambient(MapSound *a);
int mapdata_music(MapSound *m);

// --- Вода: waters[] -------------------------------------------------------------
int mapdata_water_count(void);
const MapWater *mapdata_water(int i);

// --- Команды карты: commands = [ "команда", ... ] -----------------------------
// Выполняются через консоль при загрузке карты (после построения сцены),
// можно менять cvar'ы, бинды, запускать скрипты.
#define MAP_MAX_COMMANDS 64     // потолок команд в карте
#define MAP_CMD_LEN      256    // максимум на команду
int mapdata_command_count(void);
const char *mapdata_command(int i);

// --- Cvar'ы карты: cvars = { "имя": значение, ... } ---------------------------
// Применяются при загрузке карты (после сцены, до команд).
#define MAP_MAX_CVARS 64
int mapdata_cvar_count(void);
const char *mapdata_cvar_name(int i);
float mapdata_cvar_value(int i);

// Освобождает JSON-арену. Вызывать при выходе.
void mapdata_free(void);

#endif
