/* termios.h --- stand-in for MinGW, for the simulator core's standalone
   raw console (unused here: see sys/select.h).  */
#ifndef WINCOMPAT_TERMIOS_H
#define WINCOMPAT_TERMIOS_H
struct termios { unsigned c_lflag; };
#define ICANON 1
#define ECHO 2
#define ECHOE 4
#define TCSANOW 0
static inline int tcgetattr (int fd, struct termios *t) { (void) fd; t->c_lflag = 0; return -1; }
static inline int tcsetattr (int fd, int a, const struct termios *t) { (void) fd; (void) a; (void) t; return -1; }
#endif
