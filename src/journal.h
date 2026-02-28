#ifndef LR_JOURNAL_H
#define LR_JOURNAL_H

#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>

struct lr_state;

/*
 * Dirty-position bitmap + background worker thread.
 *
 * Each bit corresponds to one parity position.  When a data block is
 * written the corresponding bit is set.  A background worker thread
 * periodically drains the bitmap by calling parity_update_position()
 * for each set bit.
 *
 * Crash-consistent journal: the bitmap is saved to bitmap_path alongside
 * each periodic metadata save.  On unmount the file is deleted (clean
 * shutdown).  On remount, if the file is found, the dirty bits are merged
 * into the in-memory bitmap so stale parity positions are recomputed.
 */
typedef struct lr_journal {
    /* Bitmap — one bit per parity position */
    uint64_t       *bitmap;
    uint32_t        bitmap_words; /* current allocated words */
    pthread_mutex_t bitmap_lock;

    /* Background worker */
    pthread_t       worker;
    int             running;
    int             processing;   /* 1 while worker is computing parity */
    pthread_cond_t  wake_cond;
    pthread_cond_t  drain_cond;   /* signalled when processing==0 and bitmap empty */
    unsigned        interval_ms;     /* default sleep between sweeps */
    unsigned        save_interval_s; /* seconds between periodic metadata+bitmap saves */

    /* Persistent crash journal */
    char            bitmap_path[PATH_MAX]; /* on-disk dirty-bitmap file; "" = disabled */

    /* Parity drain parallelism */
    unsigned        nthreads;  /* number of threads to use when draining dirty positions */

    /* Scrub / repair */
    volatile sig_atomic_t scrub_pending;  /* set by SIGUSR1 handler */
    volatile sig_atomic_t repair_pending; /* set by SIGUSR2 handler */

    struct lr_state *state;
} lr_journal;

int  journal_init(lr_journal *j, struct lr_state *s, unsigned interval_ms,
                  unsigned nthreads);
void journal_done(lr_journal *j);

/* Mark positions [start, start+count) as dirty. */
void journal_mark_dirty_range(lr_journal *j, uint32_t start, uint32_t count);

/* Block until all dirty positions have been processed. */
void journal_flush(lr_journal *j);

/*
 * Set the path for the on-disk dirty-bitmap file and load any existing
 * bitmap (crash recovery).  Call after journal_init, before fuse_main.
 */
void journal_set_bitmap_path(lr_journal *j, const char *path);

/* Request a full parity scrub (also triggerable via SIGUSR1). */
void journal_scrub_request(lr_journal *j);

/* Request a full parity repair/resync (also triggerable via SIGUSR2).
 * Like scrub, but overwrites any mismatched parity with correct values.
 * Use after adding a new parity level or to fix parity after a crash. */
void journal_repair_request(lr_journal *j);

#endif /* LR_JOURNAL_H */
