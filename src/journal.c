#include "journal.h"
#include "state.h"
#include "parity.h"
#include "metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                       */
/* ------------------------------------------------------------------ */

static void bitmap_ensure(lr_journal *j, uint32_t pos)
{
    uint32_t word = pos / 64;
    if (word >= j->bitmap_words) {
        uint32_t new_words = (word + 1) * 2;
        uint64_t *p = realloc(j->bitmap, new_words * sizeof(uint64_t));
        if (!p)
            return;
        memset(p + j->bitmap_words, 0,
               (new_words - j->bitmap_words) * sizeof(uint64_t));
        j->bitmap       = p;
        j->bitmap_words = new_words;
    }
}

static void bitmap_set(lr_journal *j, uint32_t pos)
{
    bitmap_ensure(j, pos);
    j->bitmap[pos / 64] |= (uint64_t)1 << (pos % 64);
}

/* ------------------------------------------------------------------ */
/* Persistent bitmap (crash journal)                                   */
/* ------------------------------------------------------------------ */

/*
 * On-disk format (little-endian):
 *   magic[4]         "LRBM"
 *   bitmap_words[4]  uint32_t
 *   bitmap data      uint64_t[bitmap_words]
 */

static void journal_bitmap_save(lr_journal *j)
{
    if (j->bitmap_path[0] == '\0')
        return;

    /* Copy the bitmap under lock */
    pthread_mutex_lock(&j->bitmap_lock);
    uint32_t words = j->bitmap_words;
    uint64_t *copy = NULL;
    if (words > 0 && j->bitmap) {
        copy = malloc(words * sizeof(uint64_t));
        if (copy)
            memcpy(copy, j->bitmap, words * sizeof(uint64_t));
    }
    pthread_mutex_unlock(&j->bitmap_lock);

    if (!copy || words == 0) {
        /* Nothing dirty — remove any stale file */
        unlink(j->bitmap_path);
        free(copy);
        return;
    }

    /* Skip write if all bits are zero */
    int has_bits = 0;
    for (uint32_t w = 0; w < words && !has_bits; w++)
        if (copy[w]) has_bits = 1;
    if (!has_bits) {
        unlink(j->bitmap_path);
        free(copy);
        return;
    }

    char tmp[PATH_MAX + 8]; /* +8 for ".tmp\0" with room to spare */
    snprintf(tmp, sizeof(tmp), "%s.tmp", j->bitmap_path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(copy);
        return;
    }

    const uint8_t magic[4] = { 'L', 'R', 'B', 'M' };
    if (write(fd, magic, 4) != 4 ||
        write(fd, &words, sizeof(words)) != sizeof(words) ||
        write(fd, copy, words * sizeof(uint64_t))
            != (ssize_t)(words * sizeof(uint64_t))) {
        close(fd);
        unlink(tmp);
        free(copy);
        return;
    }
    fsync(fd);
    close(fd);
    free(copy);

    if (rename(tmp, j->bitmap_path) != 0) {
        fprintf(stderr, "journal: failed to save bitmap '%s': %s\n",
                j->bitmap_path, strerror(errno));
        unlink(tmp);
    }
}

static void journal_bitmap_load(lr_journal *j)
{
    if (j->bitmap_path[0] == '\0')
        return;

    int fd = open(j->bitmap_path, O_RDONLY);
    if (fd < 0)
        return; /* no file = clean shutdown */

    uint8_t magic[4];
    uint32_t words;
    if (read(fd, magic, 4) != 4 ||
        memcmp(magic, "LRBM", 4) != 0 ||
        read(fd, &words, sizeof(words)) != sizeof(words) ||
        words == 0 || words > 0x100000 /* 64M positions max */) {
        close(fd);
        return;
    }

    uint64_t *bm = calloc(words, sizeof(uint64_t));
    if (!bm) { close(fd); return; }

    ssize_t got = read(fd, bm, words * sizeof(uint64_t));
    close(fd);

    if (got != (ssize_t)(words * sizeof(uint64_t))) {
        free(bm);
        return;
    }

    /* Merge loaded bits into the current in-memory bitmap */
    pthread_mutex_lock(&j->bitmap_lock);
    if (j->bitmap_words < words) {
        uint64_t *p = realloc(j->bitmap, words * sizeof(uint64_t));
        if (p) {
            memset(p + j->bitmap_words, 0,
                   (words - j->bitmap_words) * sizeof(uint64_t));
            j->bitmap       = p;
            j->bitmap_words = words;
        }
    }
    uint32_t limit = words < j->bitmap_words ? words : j->bitmap_words;
    for (uint32_t w = 0; w < limit; w++)
        j->bitmap[w] |= bm[w];
    pthread_mutex_unlock(&j->bitmap_lock);

    free(bm);
    fprintf(stderr,
            "journal: restored dirty bitmap from '%s' (crash recovery)\n",
            j->bitmap_path);
}

/* ------------------------------------------------------------------ */
/* Parallel parity drain                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    lr_state *state;
    uint32_t *positions;
    uint32_t  count;
    void    **v;
} parity_work_t;

static void *parity_worker_thread(void *arg)
{
    parity_work_t *w = (parity_work_t *)arg;
    lr_state      *s = w->state;
    for (uint32_t i = 0; i < w->count; i++) {
        pthread_rwlock_rdlock(&s->state_lock);
        parity_update_position(s, w->positions[i], w->v);
        pthread_rwlock_unlock(&s->state_lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Background worker                                                    */
/* ------------------------------------------------------------------ */

static int bitmap_is_empty(lr_journal *j)
{
    if (j->bitmap == NULL || j->bitmap_words == 0)
        return 1;
    for (uint32_t w = 0; w < j->bitmap_words; w++)
        if (j->bitmap[w]) return 0;
    return 1;
}

static void *worker_thread(void *arg)
{
    lr_journal *j = (lr_journal *)arg;
    lr_state   *s = j->state;

    /* Allocate scratch vector once for parity recomputation */
    unsigned nd = s->drive_count;
    unsigned np = s->parity ? s->parity->levels : 0;
    void    *freeptr = NULL;
    void   **v = NULL;

    if (nd > 0 && np > 0) {
        v = lr_alloc_vector((int)(nd + np), s->cfg.block_size, &freeptr);
    }

    time_t last_save = time(NULL);

    while (1) {
        /* Wait for wake signal or interval timeout.
         * Use min(interval_ms, save_interval_s * 1000) so the save fires
         * within save_interval_s seconds even if not signalled externally. */
        unsigned sleep_ms = j->interval_ms;
        if (j->save_interval_s > 0 && j->save_interval_s * 1000 < sleep_ms)
            sleep_ms = j->save_interval_s * 1000;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += sleep_ms / 1000;
        ts.tv_nsec += (sleep_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&j->bitmap_lock);
        pthread_cond_timedwait(&j->wake_cond, &j->bitmap_lock, &ts);

        if (!j->running) {
            pthread_mutex_unlock(&j->bitmap_lock);
            break;
        }
        pthread_mutex_unlock(&j->bitmap_lock);

        /* Periodic metadata + bitmap save — done BEFORE the swap so the saved
         * bitmap contains the dirty positions that are about to be drained.
         * This ensures crash recovery works: if the process dies after the save
         * but before (or during) the drain, those positions are re-drained on
         * the next mount. */
        if (j->save_interval_s > 0) {
            time_t now = time(NULL);
            if (now - last_save >= (time_t)j->save_interval_s) {
                pthread_rwlock_rdlock(&s->state_lock);
                metadata_save(s);
                pthread_rwlock_unlock(&s->state_lock);
                journal_bitmap_save(j);
                last_save = now;
            }
        }

        /* Atomically swap out the bitmap */
        pthread_mutex_lock(&j->bitmap_lock);
        uint64_t *old_bm    = j->bitmap;
        uint32_t  old_words = j->bitmap_words;
        j->bitmap       = NULL;
        j->bitmap_words = 0;

        /* Mark processing before releasing the lock so journal_flush
         * can't see a false "empty + idle" window between the swap and
         * the actual parity writes. */
        if (old_bm) {
            int has_bits = 0;
            for (uint32_t w = 0; w < old_words; w++)
                if (old_bm[w]) { has_bits = 1; break; }
            if (has_bits)
                j->processing = 1;
            else {
                free(old_bm);
                old_bm = NULL;
            }
        }
        pthread_mutex_unlock(&j->bitmap_lock);

        /* Process each dirty position */
        if (v && old_bm) {
            unsigned nt = j->nthreads > 0 ? j->nthreads : 1;

            if (nt <= 1) {
                /* Serial path */
                for (uint32_t w = 0; w < old_words; w++) {
                    uint64_t word = old_bm[w];
                    while (word) {
                        int bit = __builtin_ctzll(word);
                        uint32_t pos = w * 64 + (uint32_t)bit;
                        word &= word - 1;
                        pthread_rwlock_rdlock(&s->state_lock);
                        parity_update_position(s, pos, v);
                        pthread_rwlock_unlock(&s->state_lock);
                    }
                }
            } else {
                /* Parallel path: collect positions, divide across nt threads */
                uint32_t pos_count = 0;
                for (uint32_t w = 0; w < old_words; w++)
                    pos_count += (uint32_t)__builtin_popcountll(old_bm[w]);

                uint32_t *positions = pos_count
                    ? malloc(pos_count * sizeof(uint32_t)) : NULL;

                if (positions) {
                    uint32_t idx = 0;
                    for (uint32_t w = 0; w < old_words; w++) {
                        uint64_t word = old_bm[w];
                        while (word) {
                            int bit = __builtin_ctzll(word);
                            positions[idx++] = w * 64 + (uint32_t)bit;
                            word &= word - 1;
                        }
                    }
                }

                if (nt > pos_count && pos_count > 0) nt = pos_count;

                parity_work_t *works   = positions ? malloc(nt * sizeof(parity_work_t))  : NULL;
                pthread_t     *threads = positions ? malloc((nt-1) * sizeof(pthread_t))  : NULL;
                void         **fps     = positions ? calloc(nt, sizeof(void *))           : NULL;
                int            par_ok  = works && threads && fps;

                /* Allocate scratch vectors for threads 1..nt-1 */
                if (par_ok) {
                    uint32_t chunk = (pos_count + nt - 1) / nt;
                    works[0].state     = s;
                    works[0].positions = positions;
                    works[0].count     = chunk < pos_count ? chunk : pos_count;
                    works[0].v         = v;
                    for (unsigned t = 1; t < nt && par_ok; t++) {
                        uint32_t start = t * chunk;
                        uint32_t end   = start + chunk < pos_count
                                         ? start + chunk : pos_count;
                        works[t].state     = s;
                        works[t].positions = positions + start;
                        works[t].count     = end - start;
                        works[t].v = lr_alloc_vector((int)(nd + np),
                                         s->cfg.block_size, &fps[t]);
                        if (!works[t].v) par_ok = 0;
                    }
                }

                if (par_ok) {
                    /* Spawn threads 1..nt-1, run chunk 0 inline */
                    int *created = calloc(nt - 1, sizeof(int));
                    for (unsigned t = 1; t < nt; t++) {
                        if (created)
                            created[t-1] = (pthread_create(&threads[t-1], NULL,
                                            parity_worker_thread, &works[t]) == 0);
                    }
                    parity_worker_thread(&works[0]);
                    for (unsigned t = 1; t < nt; t++) {
                        if (created && created[t-1])
                            pthread_join(threads[t-1], NULL);
                        else
                            parity_worker_thread(&works[t]);
                        free(fps[t]);
                    }
                    free(created);
                } else {
                    /* Fallback: serial over positions array (or bitmap if alloc failed) */
                    for (unsigned t = 1; t < nt; t++) {
                        if (fps) free(fps[t]);
                    }
                    if (positions) {
                        for (uint32_t i = 0; i < pos_count; i++) {
                            pthread_rwlock_rdlock(&s->state_lock);
                            parity_update_position(s, positions[i], v);
                            pthread_rwlock_unlock(&s->state_lock);
                        }
                    } else {
                        for (uint32_t w = 0; w < old_words; w++) {
                            uint64_t word = old_bm[w];
                            while (word) {
                                int bit = __builtin_ctzll(word);
                                uint32_t pos = w * 64 + (uint32_t)bit;
                                word &= word - 1;
                                pthread_rwlock_rdlock(&s->state_lock);
                                parity_update_position(s, pos, v);
                                pthread_rwlock_unlock(&s->state_lock);
                            }
                        }
                    }
                }

                free(works);
                free(threads);
                free(fps);
                free(positions);
            }
            free(old_bm);
        } else {
            free(old_bm);
        }

        /* Clear processing flag and wake any flush waiters */
        pthread_mutex_lock(&j->bitmap_lock);
        j->processing = 0;
        pthread_cond_broadcast(&j->drain_cond);
        pthread_mutex_unlock(&j->bitmap_lock);

        /* Scrub / repair if requested — after the drain completes */
        if (j->repair_pending || j->scrub_pending) {
            int do_repair    = j->repair_pending;
            j->scrub_pending  = 0;
            j->repair_pending = 0;
            lr_scrub_result result;
            parity_scrub(s, &result, do_repair);
            if (do_repair)
                fprintf(stderr,
                        "repair: %u positions checked, "
                        "%u mismatches, %u fixed, %u read errors\n",
                        result.positions_checked,
                        result.parity_mismatches,
                        result.parity_fixed,
                        result.read_errors);
            else
                fprintf(stderr,
                        "scrub: %u positions checked, "
                        "%u parity mismatches, %u read errors\n",
                        result.positions_checked,
                        result.parity_mismatches,
                        result.read_errors);
        }
    }

    free(freeptr);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int journal_init(lr_journal *j, struct lr_state *s, unsigned interval_ms,
                 unsigned nthreads)
{
    memset(j, 0, sizeof(*j));
    j->state            = s;
    j->interval_ms      = interval_ms > 0 ? interval_ms : 5000;
    j->save_interval_s  = s->cfg.bitmap_interval_s > 0 ? s->cfg.bitmap_interval_s : 300;
    j->nthreads         = nthreads > 0 ? nthreads : 1;
    j->running          = 1;

    if (pthread_mutex_init(&j->bitmap_lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&j->wake_cond, NULL) != 0) {
        pthread_mutex_destroy(&j->bitmap_lock);
        return -1;
    }
    if (pthread_cond_init(&j->drain_cond, NULL) != 0) {
        pthread_cond_destroy(&j->wake_cond);
        pthread_mutex_destroy(&j->bitmap_lock);
        return -1;
    }
    if (pthread_create(&j->worker, NULL, worker_thread, j) != 0) {
        j->running = 0;
        pthread_cond_destroy(&j->drain_cond);
        pthread_cond_destroy(&j->wake_cond);
        pthread_mutex_destroy(&j->bitmap_lock);
        return -1;
    }
    return 0;
}

void journal_done(lr_journal *j)
{
    /* Signal worker to stop */
    pthread_mutex_lock(&j->bitmap_lock);
    j->running = 0;
    pthread_cond_signal(&j->wake_cond);
    pthread_mutex_unlock(&j->bitmap_lock);

    pthread_join(j->worker, NULL);

    /* Clean shutdown: remove the persistent bitmap file */
    if (j->bitmap_path[0] != '\0')
        unlink(j->bitmap_path);

    pthread_mutex_destroy(&j->bitmap_lock);
    pthread_cond_destroy(&j->wake_cond);
    pthread_cond_destroy(&j->drain_cond);
    free(j->bitmap);
    memset(j, 0, sizeof(*j));
}

void journal_mark_dirty_range(lr_journal *j, uint32_t start, uint32_t count)
{
    pthread_mutex_lock(&j->bitmap_lock);
    for (uint32_t i = 0; i < count; i++)
        bitmap_set(j, start + i);
    /* No signal here: drain is timer-driven so the periodic save fires first
     * and captures the dirty bitmap before it is drained (crash recovery).
     * Explicit drains (file close, unmount) use journal_flush() which signals. */
    pthread_mutex_unlock(&j->bitmap_lock);
}

void journal_flush(lr_journal *j)
{
    /* Kick the worker so it drains immediately */
    pthread_mutex_lock(&j->bitmap_lock);
    pthread_cond_signal(&j->wake_cond);

    /* Wait until both the bitmap is empty AND the worker has finished
     * processing the batch it swapped out. */
    while (j->processing || !bitmap_is_empty(j))
        pthread_cond_wait(&j->drain_cond, &j->bitmap_lock);

    pthread_mutex_unlock(&j->bitmap_lock);
}

void journal_set_bitmap_path(lr_journal *j, const char *path)
{
    snprintf(j->bitmap_path, sizeof(j->bitmap_path), "%s", path);
    journal_bitmap_load(j); /* restore dirty positions from previous run */
}

void journal_scrub_request(lr_journal *j)
{
    j->scrub_pending = 1;
    pthread_mutex_lock(&j->bitmap_lock);
    pthread_cond_signal(&j->wake_cond);
    pthread_mutex_unlock(&j->bitmap_lock);
}

void journal_repair_request(lr_journal *j)
{
    j->repair_pending = 1;
    pthread_mutex_lock(&j->bitmap_lock);
    pthread_cond_signal(&j->wake_cond);
    pthread_mutex_unlock(&j->bitmap_lock);
}
