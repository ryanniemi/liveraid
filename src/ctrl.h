#ifndef LR_CTRL_H
#define LR_CTRL_H

#include <pthread.h>
#include <limits.h>

/* Forward declaration; state.h is included by ctrl.c which needs full type */
struct lr_state;

/*--------------------------------------------------------------------
 * Control server: Unix domain socket listener for live rebuild.
 * The running liveraid process creates this socket so that
 * `liveraid rebuild` can rebuild a failed drive without unmounting.
 *------------------------------------------------------------------*/
typedef struct lr_ctrl {
    int              sock_fd;               /* listening fd; -1 when stopped */
    pthread_t        thread;
    int              running;
    struct lr_state *state;
    char             sock_path[PATH_MAX + 8];
} lr_ctrl;

/*
 * Start the control server.  Binds a Unix domain socket at
 * <content_paths[0]>.ctrl and spawns the accept thread.
 * Returns 0 on success, -1 on failure.
 */
int  ctrl_start(lr_ctrl *c, struct lr_state *s);

/*
 * Stop the control server.  Closes the socket (unblocks accept),
 * joins the thread, and unlinks the socket path.
 * Safe to call multiple times or when start failed (sock_fd == -1).
 */
void ctrl_stop(lr_ctrl *c);

#endif /* LR_CTRL_H */
