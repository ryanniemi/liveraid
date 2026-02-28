#ifndef LR_REBUILD_H
#define LR_REBUILD_H

/*
 * Entry point for the "liveraid rebuild -c CONFIG -d DRIVE_NAME" subcommand.
 * Reconstructs all files assigned to the named drive from parity, writing
 * them to the drive's configured directory (which must be present and writable).
 * Does not mount FUSE; runs as a standalone offline operation.
 * Returns 0 if all files were rebuilt successfully, 1 otherwise.
 */
int cmd_rebuild(int argc, char *argv[]);

#endif /* LR_REBUILD_H */
