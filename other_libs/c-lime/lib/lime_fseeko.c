#include <lime_config.h>
#include <stdio.h>

#ifndef HAVE_FSEEKO

#ifdef HAVE_DCAP
lime_misconfiguration_fseeko_not_declared;
#endif

#include "lime_fseeko.h"

off_t fseeko(MPI_File fd, MPI_Offset offset, int whence) {
  return MPI_File_seek(fd, offset, whence);
}
 
off_t ftello(MPI_File fd) {
  MPI_Offset off;
  MPI_File_get_position(fd, &off);
  return off;
}

#endif

