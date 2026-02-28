#ifndef LR_METADATA_H
#define LR_METADATA_H

#include "state.h"

/*
 * Load content file into state (file_table, file_list, pos_alloc).
 * Returns 0 on success, -1 if file missing (not an error on first run).
 */
int metadata_load(lr_state *s);

/*
 * Save state to content file atomically (write temp → fsync → rename).
 * Writes to every path in cfg.content_paths.
 * Returns 0 on success.
 */
int metadata_save(lr_state *s);

#endif /* LR_METADATA_H */
