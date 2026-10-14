// ============================================================================
//  android.c — подготовка окружения под Android (см. android.h)
// ----------------------------------------------------------------------------
//  Список файлов в APK пишет сборка (make apk) в assets/assets.txt — построчно
//  относительные пути. Каждый файл открывается через SDL_IOFromFile (на
//  Android она умеет читать APK-assets) и копируется в SDL_GetPrefPath, если
//  файла нет или размер отличается (идемпотентно: обновление APK доносит
//  только изменившиеся файлы). Затем chdir в хранилище — все fopen движка
//  работают без единого изменения.
// ============================================================================
#include "android.h"

#ifdef __ANDROID__

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// Копирует один asset в хранилище. 0 — пропущен, 1 — скопирован, -1 — ошибка.
static int extract_one(const char *rel, const char *pref, int force)
{
    char dst[600];
    snprintf(dst, sizeof dst, "%s%s", pref, rel);

    // Актуальный файл пропускаем (размер совпадает с asset'ом)
    SDL_IOStream *src = SDL_IOFromFile(rel, "rb");
    if (!src) {
        // fallback: пробуем с префиксом assets/
        char alt[600]; snprintf(alt, sizeof alt, "assets/%s", rel);
        src = SDL_IOFromFile(alt, "rb");
        if (!src) {
            SDL_Log("assets: в APK нет %s: %s", rel, SDL_GetError());
            return -1;
        }
    }
    Sint64 sz = SDL_GetIOSize(src);
    if (sz < 0) sz = 0; // некоторые потоки не сообщают размер
    struct stat st;
    if (!force && stat(dst, &st) == 0 && sz >= 0 && (Sint64)st.st_size == sz) {
        SDL_CloseIO(src);
        return 0;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        SDL_Log("assets: не открыть %s", dst);
        SDL_CloseIO(src);
        return -1;
    }
    char buf[65536];
    size_t got;
    int ok = 1;
    while ((got = SDL_ReadIO(src, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, got, out) != got) { ok = 0; break; }
    }
    if (SDL_GetIOStatus(src) == SDL_IO_STATUS_ERROR) ok = 0;
    if (fclose(out) != 0) ok = 0;
    SDL_CloseIO(src);
    if (!ok) {
        remove(dst);
        SDL_Log("assets: ошибка копирования %s", rel);
        return -1;
    }
    SDL_Log("assets: %s (%d КБ)", rel, (int)(sz / 1024));
    return 1;
}

int fr_android_prepare(void)
{
    SDL_Log("android: prepare start (SDL inited, pref?)");
    char *pref = SDL_GetPrefPath("ZaworGroup", "firerender");
    if (!pref) {
        SDL_Log("android: нет pref-пути: %s", SDL_GetError());
        return 0;
    }
    SDL_Log("android: pref=%s", pref);

    char stamp_path[600];
    snprintf(stamp_path, sizeof stamp_path, "%s.assets-version", pref);
    char wanted[64] = "unknown";
    size_t vn = 0;
    char *version = (char *)SDL_LoadFile("assets.version", &vn);
    if (version && vn > 0) {
        if (vn >= sizeof wanted) vn = sizeof wanted - 1;
        memcpy(wanted, version, vn); wanted[vn] = 0;
    }
    SDL_free(version);
    char current[64] = "";
    FILE *sf = fopen(stamp_path, "rb");
    if (sf) { fread(current, 1, sizeof current - 1, sf); fclose(sf); }
    int force = strcmp(current, wanted) != 0;

    // assets.txt генерирует make apk (список всего, что упаковано в APK)
    size_t n = 0;
    char *list = (char *)SDL_LoadFile("assets.txt", &n);
    if (!list) { // fallback: некоторые билды кладут как assets/assets.txt
        list = (char *)SDL_LoadFile("assets/assets.txt", &n);
    }
    if (!list) {
        SDL_Log("android: в APK нет assets.txt — ресурсы не распаковать (cwd=%s, err=%s)",
                pref, SDL_GetError());
        SDL_free(pref);
        return 0;
    } else {
        int copied = 0, total = 0, failed = 0;
        char *p = list, *end = list + n;
        while (p < end) {
            char *eol = memchr(p, '\n', (size_t)(end - p));
            char *line_end = eol ? eol : end;
            size_t len = (size_t)(line_end - p);
            if (len > 0 && p[len - 1] == '\r') len--;
            p[len] = 0;
            if (len > 0 && strchr(p, '/') && !strstr(p, "..")) {
                // Создаём папку (SDL_CreateDirectory строит всю иерархию)
                char dir[600];
                snprintf(dir, sizeof dir, "%s%s", pref, p);
                char *slash = strrchr(dir, '/');
                if (slash && slash != dir) {
                    *slash = 0;
                    SDL_CreateDirectory(dir);
                }
                total++;
                int rc = extract_one(p, pref, force);
                if (rc < 0) failed++; else copied += rc;
            }
            p = eol ? eol + 1 : end;
        }
        SDL_free(list);
        SDL_Log("assets: %d/%d файлов обновлено", copied, total);
        if (failed > 0) {
            SDL_Log("assets: %d ошибок — запуск отменён", failed);
            SDL_free(pref);
            return 0;
        }
        sf = fopen(stamp_path, "wb");
        int stamp_ok = sf != NULL;
        if (sf) {
            stamp_ok = fwrite(wanted, 1, strlen(wanted), sf) == strlen(wanted);
            if (fclose(sf) != 0) stamp_ok = 0;
        }
        if (!stamp_ok) {
            SDL_Log("assets: не записать маркер версии");
            SDL_free(pref);
            return 0;
        }
    }

    // Рабочая папка = хранилище: config.cfg, save.cfg и логи пишутся сюда же
    if (chdir(pref) != 0) {
        SDL_Log("android: chdir(%s) не удался", pref);
        SDL_free(pref);
        return 0;
    } else
        SDL_Log("android: рабочая папка %s", pref);
    SDL_free(pref);
    return 1;
}

#else   // !__ANDROID__

int fr_android_prepare(void)
{
    return 0;
}

#endif
