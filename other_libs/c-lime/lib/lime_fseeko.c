#include <lime_config.h>
#include <stdio.h>

#ifndef HAVE_FSEEKO

#ifdef HAVE_DCAP
lime_misconfiguration_fseeko_not_declared;
#endif

#include "lime_fseeko.h"

int fseeko(int fd, off_t offset, int whence) {
  return lseek(fd, (long)offset, whence);
}
 
off_t ftello(int fd) {
  return (off_t)lseek(fd, 0, SEEK_CUR);
}

#endif

