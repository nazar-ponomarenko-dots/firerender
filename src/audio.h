// ============================================================================
//  audio.h — звук (SDL3 audio): эффекты и фоновые петли
// ----------------------------------------------------------------------------
//  Звуки грузятся через ресурс-менеджер (пак карты -> диск -> вшитые),
//  поэтому карта может положить свои audio/*.wav в zip — они переопределят
//  движковые. Воспроизведение: несколько каналов-потоков, привязанных к
//  устройству (устройство микширует их само). Есть фоновый канал для петель
//  (ветер и т.п.).
// ============================================================================
#ifndef AUDIO_H
#define AUDIO_H

typedef struct Sound Sound;
struct WorldState;   // неполный тип: звуку нужен только указатель на карту
typedef struct WorldState WorldState;   // те же типы, что в world.h

// Инициализация звука: открывает устройство. Без звука движок работает
// (если нет устройства/файлов). Вызывать один раз после SDL_Init.
void audio_init(void);

// Каждый кадр: обслуживает фоновую петлю (доливает данные).
void audio_update(void);

// Загружает wav (через ресурсы). Кеш: один файл — один объект.
// NULL, если файла нет или он битый.
Sound *audio_load(const char *path);

// Проигрывает звук один раз. volume 0..1, pitch 0.5..2 (1 = оригинал).
// Если все каналы заняты — занимает самый старый.
void audio_play(Sound *s, float volume, float pitch);

// Фоновая петля: звук повторяется непрерывно до audio_loop_stop.
// volume 0..1.
void audio_loop_start(Sound *s, float volume);
void audio_loop_stop(void);
// Множитель громкости петли на ходу (погода: в дождь ветер громче).
// 1.0 — без изменений; применяет audio_update каждый кадр.
void audio_loop_mul_volume(float mul);

// Музыка карты: вторая независимая петля (не конфликтует с ambient).
void audio_music_start(Sound *s, float volume);
void audio_music_stop(void);

// --- Позиционный (3D) звук -----------------------------------------------------
// Источник в точке pos[3]: громкость падает с расстоянием до слушателя
// (глухо за радиусом radius), с дистанцией усиливается реверб-эхо.
// audio_3d_play — петля (повторяется, пока не audio_3d_stop);
// audio_3d_play_once — один раз (канал сам освобождается).
// Возвращает id канала (>= 0) или -1.
int audio_3d_play(Sound *s, float volume, const float pos[3], float radius);
int audio_3d_play_once(Sound *s, float volume, const float pos[3], float radius);
void audio_3d_stop(int id);
void audio_3d_stop_all(void);

// Слушатель (позиция камеры): вызывать каждый кадр из главного цикла.
void audio_set_listener(const float pos[3]);

// Мир для окклюзии: без него звук «всегда проходит насквозь». NULL — выкл.
void audio_set_world(const struct WorldState *w);

// Множитель громкости от перекрытия прямой видимости СЛУШАТЕЛЬ -> to
// (1 = открыто, меньше = за холмом/стеной). Наружу — для консоли и тестов.
float audio_occlusion_at(const float to[3]);

// Консольная команда: snd_occlude <x> <y> <z> (регистрация — в cmdreg.c).
void cmd_snd_occlude(const char *args);

// Самопроверка окклюзии на синтетической стене (FR_TEST 'Z'). Число провалов.
int audio_selftest(void);

// То же на реальной карте: лог множителя для каждого звука map.json.
// Вызывать после загрузки мира (нужны высоты и позиция игрока).
void audio_selftest_world(const WorldState *w, const float listener[3]);

// Отладка: перечисляет звуки, играющие прямо сейчас (имена через пробел).
// Возвращает число звуков. buf — строка, size — её размер.
int audio_active_names(char *buf, int size);

// Полная очистка при выходе.
void audio_cleanup(void);

#endif
