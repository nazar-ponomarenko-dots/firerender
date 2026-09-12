// ============================================================================
//  jobs.c — мини-пул рабочих потоков (см. jobs.h)
// ----------------------------------------------------------------------------
//  Постоянные работники спят на condvar; jobs_parallel_for будит их,
//  раздаёт диапазоны и ждёт завершения. Работники живут до выхода процесса
//  (движок — приложение с жизненным циклом процесса, отдельного shutdown
//  не требуется; SDL прибьёт потоки при exit).
// ============================================================================
#include "jobs.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define JOBS_MAX_WORKERS 15

typedef struct {
    SDL_Thread *thread;
    int idx;                    // номер работника
    int done;                   // 1 — текущее задание работника выполнено
} Worker;

typedef struct {
    JobsFn fn;
    void *ud;
    int first, count;           // общий диапазон [first, first+count)
    int chunk;                  // размер куска на работника
    int next;                   // курсор выдачи кусков (под мьютексом)
    int remaining;              // сколько кусков не сдано
    int epoch;                  // номер задания (борьба со спящими зря)
    int quit;
    SDL_Mutex *mx;
    SDL_Condition *cv_work, *cv_done;
} JobsState;

static Worker s_workers[JOBS_MAX_WORKERS];
static int s_workers_n = 0;
static JobsState s_st;
static int s_inited = 0;

static int worker_main(void *p)
{
    Worker *w = (Worker *)p;
    JobsState *st = &s_st;
    int my_epoch = 0;
    for (;;) {
        SDL_LockMutex(st->mx);
        while (!st->quit && st->epoch == my_epoch)
            SDL_WaitCondition(st->cv_work, st->mx);
        if (st->quit) { SDL_UnlockMutex(st->mx); break; }
        my_epoch = st->epoch;
        // Забрать свой кусок
        int i0 = -1, i1 = 0;
        if (st->next < st->count) {
            i0 = st->first + st->next;
            int len = st->chunk;
            if (i0 + len > st->first + st->count) len = st->first + st->count - i0;
            i1 = i0 + len;
            st->next += len;
        }
        SDL_UnlockMutex(st->mx);

        if (i0 >= 0) st->fn(i0, i1, st->ud);

        SDL_LockMutex(st->mx);
        w->done = 1;
        SDL_SignalCondition(st->cv_done);
        SDL_UnlockMutex(st->mx);
    }
    return 0;
}

void jobs_init(void)
{
    if (s_inited) return;
    int n = SDL_GetNumLogicalCPUCores() - 1;
    if (n < 1) n = 1;
    if (n > JOBS_MAX_WORKERS) n = JOBS_MAX_WORKERS;
    s_st.mx = SDL_CreateMutex();
    s_st.cv_work = SDL_CreateCondition();
    s_st.cv_done = SDL_CreateCondition();
    if (!s_st.mx || !s_st.cv_work || !s_st.cv_done) {
        if (s_st.cv_done) SDL_DestroyCondition(s_st.cv_done);
        if (s_st.cv_work) SDL_DestroyCondition(s_st.cv_work);
        if (s_st.mx) SDL_DestroyMutex(s_st.mx);
        s_st.mx = NULL;
        s_st.cv_work = s_st.cv_done = NULL;
        s_workers_n = 0;
        s_inited = 1;
        SDL_Log("jobs: sync fallback (не удалось создать синхронизацию: %s)",
                SDL_GetError());
        return;
    }
    s_st.quit = 0;
    s_st.epoch = 0;
    s_workers_n = 0;
    char name[32];
    for (int i = 0; i < n; i++) {
        Worker *w = &s_workers[s_workers_n];
        snprintf(name, sizeof name, "fr-worker-%d", s_workers_n);
        w->idx = s_workers_n;
        w->done = 1;
        w->thread = SDL_CreateThread(worker_main, name, w);
        if (!w->thread) {
            SDL_Log("jobs: worker %d не создан: %s", i, SDL_GetError());
            continue;
        }
        s_workers_n++;
    }
    s_inited = 1;
    SDL_Log("jobs: пул из %d работников (ядер %d)", s_workers_n,
            SDL_GetNumLogicalCPUCores());
}

int jobs_workers(void)
{
    return s_workers_n;
}

void jobs_parallel_for(int first, int count, JobsFn fn, void *ud)
{
    if (count <= 0) return;
    jobs_init();
    if (s_workers_n <= 0) {                 // пул не поднялся — fallback
        fn(first, first + count, ud);
        return;
    }
    JobsState *st = &s_st;
    int chunk = count / (s_workers_n + 1);  // вызыватель тоже работает
    if (chunk < 1) chunk = 1;

    SDL_LockMutex(st->mx);
    st->fn = fn;
    st->ud = ud;
    st->first = first;
    st->count = count;
    st->chunk = chunk;
    st->next = 0;
    st->remaining = s_workers_n;
    st->epoch++;
    for (int i = 0; i < s_workers_n; i++) s_workers[i].done = 0;
    SDL_BroadcastCondition(st->cv_work);
    SDL_UnlockMutex(st->mx);

    // Вызывающий поток берёт хвост диапазона — не бездельничает
    int main_len = count - s_workers_n * chunk;
    int m0 = first + (main_len > 0 ? s_workers_n * chunk : 0);
    if (main_len > 0) fn(m0, first + count, ud);
    else {
        // кусков меньше, чем работников: заберём что осталось в лоб
        SDL_LockMutex(st->mx);
        while (st->next < st->count) {
            int i0 = st->first + st->next;
            int len = st->chunk;
            if (i0 + len > st->first + st->count) len = st->first + st->count - i0;
            st->next += len;
            SDL_UnlockMutex(st->mx);
            fn(i0, i0 + len, ud);
            SDL_LockMutex(st->mx);
        }
        SDL_UnlockMutex(st->mx);
    }

    // Дождаться работников (кто не получил куска — сразу done)
    SDL_LockMutex(st->mx);
    for (;;) {
        int all = 1;
        for (int i = 0; i < s_workers_n; i++)
            if (!s_workers[i].done) { all = 0; break; }
        if (all) break;
        SDL_WaitConditionTimeout(st->cv_done, st->mx, 100.0f);
    }
    SDL_UnlockMutex(st->mx);
}
