// ============================================================================
//  console.h — консоль разработчика (как в GoldSrc/X-Ray)
// ----------------------------------------------------------------------------
//  Клавиша ` (~) — открыть/закрыть. Пока открыта: курсор мыши виден,
//  ввод текста работает (SDL text input), игра не реагирует на клавиши.
//
//  Ввод строки: ←/→ — курсор, Home/End, Delete, Backspace (по UTF-8),
//  Ctrl+V — вставить из буфера, Ctrl+C — копировать, Ctrl+A/E — в начало/
//  конец, Ctrl+U/K — стереть до/после курсора, Ctrl+W — стереть слово.
//  Tab — дополнение имени команды/переменной (цикл по совпадениям).
//  Прокрутка вывода: колесо мыши, PgUp/PgDn, Home/End.
//  Enter — выполнить, ↑/↓ — история, Esc — закрыть.
//
//  Вывод SDL_Log попадает в консоль (cvar "developer"). Команды
//  регистрируются через console_add_command — так позже Lua будет
//  регистрировать свои. Встроенные команды: help, cvarlist, set, exec,
//  clear, cfg, echo, quit, version.
// ============================================================================
#ifndef CONSOLE_H
#define CONSOLE_H
#include <SDL3/SDL.h>

typedef void (*ConsoleCmdFn)(const char *args);

#define CON_MAX_COMMANDS 64

// Флаг "запрошен выход" — ставит команда quit, читает главный цикл.
extern int g_quit_request;

// Инициализация: cvar'ы по умолчанию, встроенные команды, перехват
// SDL_Log в кольцо консоли. Вызывать один раз после создания GL-контекста.
void console_init(void);

// Открыть/закрыть (клавиша `). Шлёт EV_CONSOLE_OPENED/EV_CONSOLE_CLOSED.
void console_toggle(void);
int console_active(void);

// Отдать консоли событие SDL (текстовый ввод, клавиши). Возвращает 1, если
// событие съедено консолью — главный цикл не должен обрабатывать его дальше.
int console_event(const SDL_Event *ev);

// Печать строки в консоль (как printf).
void console_print(const char *fmt, ...);

// Регистрация команды. name — имя, help — подсказка, fn — обработчик
// (args — остаток строки после имени команды).
int console_add_command(const char *name, const char *help, ConsoleCmdFn fn);

// Выполнить строку команды (Enter или из кода).
void console_exec(const char *line);

// Рисует консоль (панель, строки вывода со скроллбеком, строка ввода
// с курсором). Вызывать из главного цикла ПОСЛЕ отрисовки UI.
void console_render(int vw, int vh);

// Автозагрузка config.cfg при старте и сохранение при выходе.
void console_autoload(void);
void console_save_config(void);

// Самопроверка: команды, cvar'ы, ввод (события SDL), история, Tab.
// Запускается FR_TEST=x; печатает результат через SDL_Log.
void console_selftest(void);

#endif
