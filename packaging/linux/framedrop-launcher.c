// What FrameDrop starts when you press Play on UTMTFTM. FrameDrop only accepts a real program as the thing to start,
// not a shell script, so this just runs install-utmtftm.sh from the same folder, which does the work.
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  char dir[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
  if (n <= 0) {
    perror("UTMTFTM: can't find my own folder");
    return 1;
  }
  dir[n] = 0;
  char *slash = strrchr(dir, '/');
  if (slash) *slash = 0;

  char script[PATH_MAX + 32];
  snprintf(script, sizeof script, "%s/install-utmtftm.sh", dir);
  char *args[8] = {"sh", script};
  for (int i = 1; i < argc && i < 6; i++) args[i + 1] = argv[i];
  execv("/bin/sh", args);
  perror("UTMTFTM: can't start /bin/sh");
  return 1;
}
