#ifndef LIME_FSEEKO_H
#define LIME_FSEEKO_H

#include "lime_config.h"

#ifndef HAVE_FSEEKO

/* If fseeko, ftell is not defined then define our own versions of it
   which just call fseek and ftell but keep the same interface */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

off_t fseeko(MPI_File fd, off_t offset, int whence);
off_t ftello(MPI_File fd);

#else
#include <stdio.h>
#endif

#endif
