#include <lime_config.h>
#include <stdio.h>

#ifndef HAVE_FSEEKO

#ifdef HAVE_DCAP
lime_misconfiguration_fseeko_not_declared;
#endif

#include "lime_fseeko.h"

off_t fseeko(int fd, off_t offset, int whence) {
  return lseek64(fd, (long)offset, whence);
}
 
off_t ftello(int fd) {
  return (off_t)lseek64(fd, 0, SEEK_CUR);
}

#endif

