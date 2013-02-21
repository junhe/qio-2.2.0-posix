#include <lime_config.h>
#include <lime.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>

#include "lime_binary_header.h"
#include "lime_utils.h"

#include "lime_fseeko.h"

#undef LIME_DEBUG

off_t mpiftello(MPI_File fd) {
  MPI_Offset off;
  MPI_File_get_position(fd, &off);
  return off;
}


/* Forward declaration */
int write_lime_record_binary_header(MPI_File fd, LimeRecordHeader *h);
int skip_lime_record_binary_header(MPI_File fh);
int skipWriterBytes(LimeWriter *w, off_t bytes_to_skip);

LimeWriter* limeCreateWriter(MPI_File fd)
{
  LimeWriter* ret_val;

#ifdef LIME_DEBUG
  fprintf(stderr, "Initialising LIME Generator\n");
#endif
  ret_val = (LimeWriter *)malloc(sizeof(LimeWriter));
  if( ret_val == (LimeWriter *)NULL ) { 
    return NULL;
  }

  ret_val->mpifh = fd;
  ret_val->isLastP = 0;
  ret_val->first_record = 1;
  ret_val->last_written = 0;
  ret_val->header_nextP = 1;
  ret_val->bytes_left = 0;
  ret_val->bytes_total = 0;
  ret_val->rec_ptr = 0;
  ret_val->rec_start = 0;
  ret_val->bytes_pad = 0;
  return ret_val;
}

int limeDestroyWriter(LimeWriter *s)
{
#ifdef LIME_DEBUG
  fprintf(stderr, "Closing Lime Generator\n");
#endif

#if 0
  LimeRecordHeader *h;
  n_uint64_t nbytes = 0;

  if( s->last_written != 1 ) { 

#ifdef LIME_DEBUG
    fprintf(stderr, "Writing empty last record\n");
#endif
    /* Writing a last empty record */
    h = limeCreateHeader(0, 0,
			 "", 
			 0);

    if( h == (LimeRecordHeader *)NULL ) { 
      fprintf(stderr, "Unable to close LIME\n");
      return LIME_ERR_CLOSE;
    }

    limeWriteRecordHeader(h, s);
    nbytes = 0;
    /* Take care of any unfinished record padding */
    limeWriteRecordData(NULL,&nbytes,s); 
    limeDestroyHeader(h);
  }
#endif

  free(s);
  return LIME_SUCCESS;
}


int limeWriteRecordHeader( LimeRecordHeader *props, LimeWriter *d)
{

  int ret_val;

#ifdef LIME_DEBUG 
  fprintf(stderr, "In limeWriteRecordHeader\n");
  fflush(stderr);
#endif

  if( d == (LimeWriter *)NULL ) { 
#ifdef LIME_DEBUG
    fprintf(stderr, "d is NULL\n");
    fflush(stderr);
#endif
    return LIME_ERR_PARAM;
  }

  if( props == (LimeRecordHeader *)NULL ) { 
#ifdef LIME_DEBUG
    fprintf(stderr, "props is NULL\n");
    fflush(stderr);
#endif
    return LIME_ERR_PARAM;
  }

  /* Some consistency checks ... */

  /* Make sure header is expected now */
  if( d->header_nextP != 1 ) { 
    return LIME_ERR_HEADER_NEXT;
  }

  /* If last record ended a message, this one must begin a new one */
  /* If last record did not end a message, this one must not begin one */
  /* Since we allow appending to a file and we don't reread it to check
     the state of the last flag, we don't do this check for the first
     record written. */
  if(  d->first_record != 1 && d->isLastP != props->MB_flag )
    return LIME_ERR_MBME;

  ret_val = write_lime_record_binary_header(d->mpifh, props);

  /* Set new writer state */
  d->isLastP      = props->ME_flag;
  d->first_record = props->MB_flag;
  d->bytes_left   = props->data_length;
  d->bytes_total  = props->data_length;
  d->rec_ptr      = 0;
  d->rec_start    = mpiftello(d->mpifh);
  d->bytes_pad    = lime_padding(props->data_length);
  d->header_nextP = 0;

  return ret_val;
 
}

/* Write data. */
int limeWriteRecordData( void *source, n_uint64_t *nbytes, LimeWriter* d)
{
  n_uint64_t bytes_to_write;
  int ret_val;

#ifdef LIME_DEBUG
  fprintf(stderr, "In LimeWriteRecordData\n");
#endif

  if ( d->header_nextP == 1 ) { 
    *nbytes=0;
    return LIME_ERR_PARAM;
  }

  if( *nbytes > 0 ) {
    /* If we want to write more than there is room for in the 
       current record -- then we simply truncate to how much
       there is still room for. We adjust the nbytes for return
       accordingly */
    if (*nbytes + d->rec_ptr > d->bytes_total ) { 
      bytes_to_write = d->bytes_total - d->rec_ptr;
    }
    else {
      bytes_to_write = *nbytes;
    }
    
    /* Try to write so many bytes */
    if((size_t)bytes_to_write != bytes_to_write){
      printf("limeWriteRecordData: Can't write %llu bytes\n",
	     (unsigned long long)bytes_to_write);
      return LIME_ERR_WRITE;
    }
    //ret_val = DCAP(fwrite)((const void *)source, sizeof(unsigned char), 
    //		     (size_t)bytes_to_write, d->fp);
    MPI_Status status;
    ret_val = MPI_File_write( d->mpifh, 
                                         (const void *)source, 
                             sizeof(unsigned char)*(size_t)bytes_to_write,
                             MPI_CHAR,
                             &status                             
				);
    
    *nbytes = sizeof(unsigned char)*(size_t)bytes_to_write;
    
    if( ret_val != MPI_SUCCESS )
      return LIME_ERR_WRITE;
    
    /* We succeeded */
    d->bytes_left -= bytes_to_write;
    d->rec_ptr += bytes_to_write;
    
  }

  /* Kept for compatibility -- it is not necessary to call
     limeWriterCloseRecord if the record is written sequentially up to
     the very end.  Otherwise, an explicit call is necessary. */

  if( d->bytes_left == 0 )
    limeWriterCloseRecord(d);

  return LIME_SUCCESS;
}
  
/* Advance to end of current record */
/* We need this to close out a record when we have more than one node
   writing to the same file or we seek and write in random access */

int limeWriterCloseRecord(LimeWriter *d)
{

  off_t seek_cur;
  MPI_Status status;
  size_t pad;
  unsigned char padbuf[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  int ret_val;

  /* Should we be writing a header instead now? */
  /* (If so, we have already closed the record) */
  if(d->header_nextP){
    /* Skip to the header position */
    //status = DCAPL(fseeko)(d->fp, d->rec_start + d->bytes_total + d->bytes_pad,
    //	    SEEK_SET);
    ret_val = MPI_File_seek(d->mpifh, d->rec_start + d->bytes_total + d->bytes_pad, MPI_SEEK_SET);
    if(ret_val != MPI_SUCCESS){
      printf("fseek returned %lld seekto %lld sizeof(off_t):%d\n",
            ret_val, d->rec_start + d->bytes_total + d->bytes_pad, sizeof(off_t));fflush(stdout);
      fflush(stderr);
      return LIME_ERR_SEEK;
    }
    return LIME_SUCCESS;
  }

  /* Advance to end of record */
  seek_cur = d->bytes_total - d->rec_ptr;
  skipWriterBytes(d, seek_cur);

  /* Stuff to do here */
  /* Padding */
  pad = lime_padding(d->bytes_total);
  if( pad > 0 ) { 
    //ret_val = DCAP(fwrite)((const void *)padbuf, sizeof(unsigned char),
    //	     pad, d->fp);
    //ret_val = write(d->fd, (const void *)padbuf, sizeof(unsigned char)*pad);
    ret_val = MPI_File_write(d->mpifh, 
                                        (const void *)padbuf,
                                        sizeof(unsigned char)*pad,
                                        MPI_CHAR,
                                        &status);
    if( ret_val != MPI_SUCCESS )
      return LIME_ERR_WRITE;
  }
  
  d->header_nextP = 1;  /* Next thing to come is a header */
  
  if( d->isLastP == 1 ) {
    d->last_written = 1;
  }

  return LIME_SUCCESS;
}

int skip_lime_record_binary_header(MPI_File fh)
{
  off_t status;

  //status = DCAPL(fseeko)(fp, (off_t)LIME_HDR_LENGTH, SEEK_CUR);
  status = MPI_File_seek(fh, (off_t)LIME_HDR_LENGTH, MPI_SEEK_CUR);
  if(status != MPI_SUCCESS){
    printf("fseek returned %lld seekto %lld sizeof(off_t):%d\n",
            status, (off_t)LIME_HDR_LENGTH, sizeof(off_t));
    fflush(stdout);    
    return LIME_ERR_SEEK;
  }
  return LIME_SUCCESS;
}


int write_lime_record_binary_header(MPI_File fh, LimeRecordHeader *h)
{

  int i;
  int ret_val;
  MPI_Status status;

#ifdef LIME_DEBUG
  fprintf(stderr, "In write_lime_record_binary_header\n");
#endif

  /* Clear header */
  for(i = 0; i < MAX_HDR64; i++)lime_header.int64[i] = 0;

  /* Load values, converting integers to big endian if needed */
  *lime_hdr_magic_no = big_endian_long((n_uint32_t)LIME_MAGIC_NO);
  *lime_hdr_version  = big_endian_short((n_uint16_t)h->lime_version);

  /* MB flag. */
  if( h->MB_flag == 1 ) { 
    *lime_hdr_mbme = ( *lime_hdr_mbme | MB_MASK );
  }

  /* ME flag */
  if( h->ME_flag == 1 ) { 
    *lime_hdr_mbme = ( *lime_hdr_mbme | ME_MASK );
  }

  /* Data length */
  *lime_hdr_data_len = 
    big_endian_long_long((n_uint64_t)h->data_length);

  /* Record type string - trailing nulls  */
  strncpy((char*)lime_hdr_rec_type,h->type,MAX_LIME_HDR_REC_TYPE);

  /* Force a null termination */
  lime_hdr_rec_type[MAX_LIME_HDR_REC_TYPE] = '\0';

  /* Write the header */
  //ret_val = DCAP(fwrite)((const void *)lime_header.int64, 
  //			 sizeof(n_uint64_t), MAX_HDR64, fp);
  ret_val = MPI_File_write(fh, 
                                      (const void *)lime_header.int64, 
                                      sizeof(n_uint64_t) *MAX_HDR64,
                                      MPI_CHAR,
                                      &status);

  if( ret_val != MPI_SUCCESS  ) { 
    return LIME_ERR_WRITE;
  }

  return LIME_SUCCESS;
}  

/* Skip bytes within the current record.  Positive and negative
   offsets are allowed.  If the skip takes us out of bounds of the
   data in the current record, skip to nearest boundary of the record
   and return an error. */

int skipWriterBytes(LimeWriter *w, off_t bytes_to_skip)
{

  off_t status;
  n_uint64_t new_rec_ptr;  /* The new record pointer */
  n_uint64_t offset;

  /* Ignore zero. */
  if(bytes_to_skip == 0)return LIME_SUCCESS;

  new_rec_ptr = w->rec_ptr + bytes_to_skip;

  /* Prevent skip past the end of the data */
  /* In this case set the new pointer to the end of the record */
  if( new_rec_ptr > w->bytes_total ){
    new_rec_ptr = w->bytes_total;
    printf("Seeking past end of data\n");fflush(stdout);
    status = LIME_ERR_SEEK;
  }

  /* Prevent skips before the beginning of the data */
  /* In this case set the new pointer to the beginning of the record */
  if(new_rec_ptr < 0){
    new_rec_ptr = 0;
    printf("Seeking before beginning end of data\n");fflush(stdout);
    status = LIME_ERR_SEEK;
  }

  /* Seek */
  offset = w->rec_start + new_rec_ptr;
  if((off_t)offset != offset){
    printf("skipWriterBytes: Can't seek to %llu. off_t type too small\n",
	   (unsigned long long)offset);
    return LIME_ERR_SEEK;
  }
  status = MPI_File_seek(w->mpifh, (off_t)offset, MPI_SEEK_SET);

  if(status != MPI_SUCCESS){
    printf("fseek returned %lld seekto %lld sizeof(off_t):%d\n",
            status, offset, sizeof(off_t));fflush(stdout);
    return LIME_ERR_SEEK;
  }

  /* Update the writer state */
  w->rec_ptr = new_rec_ptr;
  
  return LIME_SUCCESS;
}

int limeWriterSeek(LimeWriter *w, off_t offset, int whence){
  off_t status;

  if(whence == SEEK_CUR){
    status = skipWriterBytes(w, offset);
  }
  else if(whence == SEEK_SET){
    status = skipWriterBytes(w, offset - w->rec_ptr);
  }
  else{
    fprintf(stderr, "limeWriterSeek code %x not implemented yet\n",whence);  
    status = LIME_ERR_WRITE;
  }
  return status;
}

/* Manipulator to set the writer to a prescribed state.  We use this
   functionality to synchronize multinode writing to the same file.
   To synchronize, have the master node call limeCreateWriter and
   limeWriteRecordHeader.  Have the master node broadcast the
   resulting LimeWriter structure to the secondary nodes.  Have each
   secondary node call wdest = limeCreateWriter and then call this
   procedure with wsrc, then broadcast master node's writer. */

int limeWriterSetState(LimeWriter *wdest, LimeWriter *wsrc ){
  off_t status;

  /* Set wdest writer state from wsrc */
  /* We do not copy the file pointer member fp */
  wdest->first_record = wsrc->first_record ;
  wdest->last_written = wsrc->last_written ;
  wdest->header_nextP = wsrc->header_nextP ;
  wdest->bytes_total  = wsrc->bytes_total  ;
  wdest->bytes_left   = wsrc->bytes_left   ;
  wdest->rec_ptr      = wsrc->rec_ptr      ;
  wdest->rec_start    = wsrc->rec_start    ;
  wdest->bytes_pad    = wsrc->bytes_pad    ;
  wdest->isLastP      = wsrc->isLastP      ;

  /* Now make the system state agree with the writer state */
  status = MPI_File_seek(wdest->mpifh, wdest->rec_start + wdest->rec_ptr, 
			 MPI_SEEK_SET);
  if(status != MPI_SUCCESS){
    printf("fseek returned %lld", status);
    return LIME_ERR_SEEK;
  }
  return LIME_SUCCESS;
}
