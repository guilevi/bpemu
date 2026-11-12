/* sys/select.h --- MinGW has none.  The simulator core (as librc8650's
   patch leaves it) reads its standalone console through select(); the
   emulator replaces that path with its own receive hook, so this only has
   to compile: select() reports nothing ready.  */
#ifndef WINCOMPAT_SYS_SELECT_H
#define WINCOMPAT_SYS_SELECT_H
#include <sys/time.h>
typedef struct { int n; } wc_fd_set;
#define fd_set wc_fd_set
#define FD_ZERO(s) ((s)->n = 0)
#define FD_SET(fd, s) ((void) (fd), (s)->n = 1)
static inline int
wc_select (int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
{
  (void) n; (void) r; (void) w; (void) e; (void) t;
  return 0;
}
#define select wc_select
#endif
