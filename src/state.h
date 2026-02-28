#ifndef LR_STATE_H
#define LR_STATE_H

#include "config.h"
#include "alloc.h"

#include <stdint.h>
#include <pthread.h>
#include <limits.h>
#include <sys/types.h>

#include "lr_hash.h"
#include "lr_list.h"

/*--------------------------------------------------------------------
 * Forward declarations
 *------------------------------------------------------------------*/
struct lr_parity_handle;
struct lr_journal;
struct lr_ctrl;

/*--------------------------------------------------------------------
 * Per-drive runtime info
 *------------------------------------------------------------------*/
typedef struct lr_drive {
    char             name[64];
    char             dir[PATH_MAX];    /* absolute path including trailing '/' */
    unsigned         idx;              /* index in state.drives[] */
    uint32_t         rr_seq;           /* round-robin counter */
    lr_pos_allocator pos_alloc;        /* per-drive parity position allocator */
} lr_drive;

/*--------------------------------------------------------------------
 * Per-file record
 *------------------------------------------------------------------*/
typedef struct lr_file {
    char     vpath[PATH_MAX];       /* virtual path, e.g. "/movies/foo.mkv" */
    char     real_path[PATH_MAX];   /* absolute path on the data drive */
    unsigned drive_idx;             /* index in state.drives[] */
    int64_t  size;                  /* bytes */
    uint32_t block_count;
    uint32_t parity_pos_start;      /* blocks occupy [start, start+count) */
    time_t   mtime_sec;
    long     mtime_nsec;
    mode_t   mode;              /* full st_mode, e.g. S_IFREG | 0644 */
    uid_t    uid;
    gid_t    gid;
    int      open_count;        /* number of open FUSE file handles, guarded by state_lock */

    lr_hash_node  vpath_node;       /* embedded node for file_table */
    lr_list_node  list_node;        /* embedded node for file_list */
} lr_file;

/*--------------------------------------------------------------------
 * Per-directory record (explicitly mkdir'd or had metadata changed)
 *------------------------------------------------------------------*/
typedef struct lr_dir {
    char     vpath[PATH_MAX];
    mode_t   mode;              /* full st_mode including S_IFDIR */
    uid_t    uid;
    gid_t    gid;
    time_t   mtime_sec;
    long     mtime_nsec;

    lr_hash_node  vpath_node;   /* embedded node for dir_table */
    lr_list_node  list_node;    /* embedded node for dir_list */
} lr_dir;

/*--------------------------------------------------------------------
 * Position-index entry (for parity worker lookup)
 *------------------------------------------------------------------*/
typedef struct {
    uint32_t  pos_start;
    uint32_t  block_count;
    lr_file  *file;
} lr_pos_entry;

/*--------------------------------------------------------------------
 * Central state singleton
 *------------------------------------------------------------------*/
typedef struct lr_state {
    lr_config         cfg;
    lr_drive          drives[LR_DRIVE_MAX];
    unsigned          drive_count;

    lr_hash           file_table;   /* vpath → lr_file* */
    lr_list           file_list;    /* all lr_file* for iteration */

    lr_hash           dir_table;    /* vpath → lr_dir* (explicit dirs) */
    lr_list           dir_list;     /* all lr_dir* for iteration/save */

    struct lr_parity_handle *parity;
    struct lr_journal        *journal;

    pthread_rwlock_t  state_lock;

    /* Per-drive sorted position index for parity worker lookup */
    lr_pos_entry     *pos_index[LR_DRIVE_MAX];
    uint32_t          pos_index_count[LR_DRIVE_MAX];

    /* Round-robin drive selection counter */
    unsigned          rr_next;

    /* Control server (Unix domain socket for live rebuild), NULL if not started */
    struct lr_ctrl   *ctrl;
} lr_state;

extern lr_state *g_state;

/*--------------------------------------------------------------------
 * Lifecycle
 *------------------------------------------------------------------*/
int  state_init(lr_state *s, const lr_config *cfg);
void state_done(lr_state *s);

/*--------------------------------------------------------------------
 * File table operations (caller must hold appropriate lock)
 *------------------------------------------------------------------*/

/* Insert file into table and list. Takes ownership. */
void state_insert_file(lr_state *s, lr_file *f);

/* Remove and return file from table/list (caller frees). */
lr_file *state_remove_file(lr_state *s, const char *vpath);

/* Lookup by vpath. Returns NULL if not found. */
lr_file *state_find_file(lr_state *s, const char *vpath);

/*--------------------------------------------------------------------
 * Directory table operations (caller must hold appropriate lock)
 *------------------------------------------------------------------*/

void     state_insert_dir(lr_state *s, lr_dir *d);
lr_dir  *state_find_dir(lr_state *s, const char *vpath);
lr_dir  *state_remove_dir(lr_state *s, const char *vpath);

/*--------------------------------------------------------------------
 * Drive selection for new files
 *------------------------------------------------------------------*/
unsigned state_pick_drive(lr_state *s);

/*--------------------------------------------------------------------
 * Block-count helper
 *------------------------------------------------------------------*/
static inline uint32_t blocks_for_size(uint64_t size, uint32_t block_size)
{
    if (size == 0) return 0;
    return (uint32_t)(size / block_size + (size % block_size != 0));
}

/*--------------------------------------------------------------------
 * Position index (rebuild after mutations)
 *------------------------------------------------------------------*/
void state_rebuild_pos_index(lr_state *s, unsigned drive_idx);

/* Binary search: find file on drive_idx that has data at position pos. */
lr_file *state_find_file_at_pos(lr_state *s, unsigned drive_idx, uint32_t pos);

#endif /* LR_STATE_H */
