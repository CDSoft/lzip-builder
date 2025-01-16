/* Plzip - Massively parallel implementation of lzip
   Copyright (C) 2009 Laszlo Ersek.
   Copyright (C) 2009-2025 Antonio Diaz Diaz.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <lzlib.h>

#include "lzip.h"
#include "lzip_index.h"


/* This code is based on a patch by Hannes Domani, <ssbssa@yahoo.de> to make
   possible compiling plzip under MS Windows (with MINGW compiler).
*/
#if defined __MSVCRT__ && defined WITH_MINGW
#include <windows.h>
#warning "Parallel I/O is not guaranteed to work on Windows."

ssize_t pread( int fd, void *buf, size_t count, uint64_t offset )
  {
  OVERLAPPED o = {0,0,0,0,0};
  HANDLE fh = (HANDLE)_get_osfhandle(fd);
  DWORD bytes;
  BOOL ret;

  if( fh == INVALID_HANDLE_VALUE ) { errno = EBADF; return -1; }
  o.Offset = offset & 0xffffffff;
  o.OffsetHigh = (offset >> 32) & 0xffffffff;
  ret = ReadFile( fh, buf, (DWORD)count, &bytes, &o );
  if( !ret ) { errno = EIO; return -1; }
  return (ssize_t)bytes;
  }

ssize_t pwrite( int fd, const void *buf, size_t count, uint64_t offset )
  {
  OVERLAPPED o = {0,0,0,0,0};
  HANDLE fh = (HANDLE)_get_osfhandle(fd);
  DWORD bytes;
  BOOL ret;

  if( fh == INVALID_HANDLE_VALUE ) { errno = EBADF; return -1; }
  o.Offset = offset & 0xffffffff;
  o.OffsetHigh = (offset >> 32) & 0xffffffff;
  ret = WriteFile(fh, buf, (DWORD)count, &bytes, &o);
  if( !ret ) { errno = EIO; return -1; }
  return (ssize_t)bytes;
  }

#endif	// __MSVCRT__


/* Return the number of bytes really read.
   If (value returned < size) and (errno == 0), means EOF was reached.
*/
int preadblock( const int fd, uint8_t * const buf, const int size,
                const long long pos )
  {
  int sz = 0;
  errno = 0;
  while( sz < size )
    {
    const int n = pread( fd, buf + sz, size - sz, pos + sz );
    if( n > 0 ) sz += n;
    else if( n == 0 ) break;				// EOF
    else if( errno != EINTR ) break;
    errno = 0;
    }
  return sz;
  }


/* Return the number of bytes really written.
   If (value returned < size), it is always an error.
*/
int pwriteblock( const int fd, const uint8_t * const buf, const int size,
                 const long long pos )
  {
  int sz = 0;
  errno = 0;
  while( sz < size )
    {
    const int n = pwrite( fd, buf + sz, size - sz, pos + sz );
    if( n > 0 ) sz += n;
    else if( n < 0 && errno != EINTR ) break;
    errno = 0;
    }
  return sz;
  }


void decompress_error( LZ_Decoder * const decoder,
                       const Pretty_print & pp,
                       Shared_retval & shared_retval, const int worker_id )
  {
  const LZ_Errno errcode = LZ_decompress_errno( decoder );
  const int retval = ( errcode == LZ_header_error || errcode == LZ_data_error ||
                       errcode == LZ_unexpected_eof ) ? 2 : 1;
  if( !shared_retval.set_value( retval ) ) return;
  pp();
  if( verbosity >= 0 )
    std::fprintf( stderr, "%s in worker %d\n", LZ_strerror( errcode ),
                  worker_id );
  }


void show_results( const unsigned long long in_size,
                   const unsigned long long out_size,
                   const unsigned dictionary_size, const bool testing )
  {
  if( verbosity >= 2 )
    {
    if( verbosity >= 4 ) show_header( dictionary_size );
    if( out_size == 0 || in_size == 0 )
      std::fputs( "no data compressed. ", stderr );
    else
      std::fprintf( stderr, "%6.3f:1, %5.2f%% ratio, %5.2f%% saved. ",
                    (double)out_size / in_size,
                    ( 100.0 * in_size ) / out_size,
                    100.0 - ( ( 100.0 * in_size ) / out_size ) );
    if( verbosity >= 3 )
      std::fprintf( stderr, "%9llu out, %8llu in. ", out_size, in_size );
    }
  if( verbosity >= 1 ) std::fputs( testing ? "ok\n" : "done\n", stderr );
  }


namespace {

struct Worker_arg
  {
  const Lzip_index * lzip_index;
  const Pretty_print * pp;
  Shared_retval * shared_retval;
  int infd;
  int num_workers;
  int outfd;
  int worker_id;
  bool nocopy;		// avoid copying decompressed data when testing
  void assign( const Lzip_index & li, const Pretty_print & pp_,
               Shared_retval & sr, const int ifd, const int nw,
               const int ofd, const int wi, const bool nc )
    { lzip_index = &li; pp = &pp_; shared_retval = &sr; infd = ifd;
      num_workers = nw; outfd = ofd; worker_id = wi; nocopy = nc; }
  };


/* Read members from input file, decompress their contents, and write to
   output file the data produced.
*/
extern "C" void * dworker( void * arg )
  {
  const Worker_arg & tmp = *(const Worker_arg *)arg;
  const Lzip_index & lzip_index = *tmp.lzip_index;
  const Pretty_print & pp = *tmp.pp;
  Shared_retval & shared_retval = *tmp.shared_retval;
  const int worker_id = tmp.worker_id;
  const int num_workers = tmp.num_workers;
  const int infd = tmp.infd;
  const int outfd = tmp.outfd;
  const bool nocopy = tmp.nocopy;
  const int buffer_size = 65536;

  uint8_t * const ibuffer = new( std::nothrow ) uint8_t[buffer_size];
  uint8_t * const obuffer =
    nocopy ? 0 : new( std::nothrow ) uint8_t[buffer_size];
  LZ_Decoder * const decoder = LZ_decompress_open();
  if( !ibuffer || ( !nocopy && !obuffer ) || !decoder ||
      LZ_decompress_errno( decoder ) != LZ_ok )
    { if( shared_retval.set_value( 1 ) ) { pp( mem_msg ); } goto done; }

  for( long i = worker_id; i < lzip_index.members(); i += num_workers )
    {
    long long data_pos = lzip_index.dblock( i ).pos();
    long long data_rest = lzip_index.dblock( i ).size();
    long long member_pos = lzip_index.mblock( i ).pos();
    long long member_rest = lzip_index.mblock( i ).size();

    while( member_rest > 0 )
      {
      if( shared_retval() ) goto done;	// other worker found a problem
      while( LZ_decompress_write_size( decoder ) > 0 )
        {
        const int size = std::min( LZ_decompress_write_size( decoder ),
                    (int)std::min( (long long)buffer_size, member_rest ) );
        if( size > 0 )
          {
          if( preadblock( infd, ibuffer, size, member_pos ) != size )
            { if( shared_retval.set_value( 1 ) )
                { pp(); show_error( "Read error", errno ); } goto done; }
          member_pos += size;
          member_rest -= size;
          if( LZ_decompress_write( decoder, ibuffer, size ) != size )
            internal_error( "library error (LZ_decompress_write)." );
          }
        if( member_rest <= 0 ) { LZ_decompress_finish( decoder ); break; }
        }
      while( true )			// write decompressed data to file
        {
        const int rd = LZ_decompress_read( decoder, obuffer, buffer_size );
        if( rd < 0 )
          { decompress_error( decoder, pp, shared_retval, worker_id );
            goto done; }
        if( rd > 0 && outfd >= 0 )
          {
          const int wr = pwriteblock( outfd, obuffer, rd, data_pos );
          if( wr != rd )
            {
            if( shared_retval.set_value( 1 ) ) { pp();
              if( verbosity >= 0 )
                std::fprintf( stderr, "Write error in worker %d: %s\n",
                              worker_id, std::strerror( errno ) ); }
            goto done;
            }
          }
        if( rd > 0 )
          {
          data_pos += rd;
          data_rest -= rd;
          }
        if( LZ_decompress_finished( decoder ) == 1 )
          {
          if( data_rest != 0 )
            internal_error( "final data_rest is not zero." );
          LZ_decompress_reset( decoder );	// prepare for next member
          break;
          }
        if( rd == 0 ) break;
        }
      }
    show_progress( lzip_index.mblock( i ).size() );
    }
done:
  if( obuffer ) { delete[] obuffer; } delete[] ibuffer;
  if( LZ_decompress_member_position( decoder ) != 0 &&
      shared_retval.set_value( 1 ) )
    pp( "Error, some data remains in decoder." );
  if( LZ_decompress_close( decoder ) < 0 && shared_retval.set_value( 1 ) )
    pp( "LZ_decompress_close failed." );
  return 0;
  }

} // end namespace


// start the workers and wait for them to finish
int decompress( const unsigned long long cfile_size, int num_workers,
                const int infd, const int outfd, const Cl_options & cl_opts,
                const Pretty_print & pp, const int debug_level,
                const int in_slots, const int out_slots, const bool from_stdin,
                const bool infd_isreg, const bool one_to_one )
  {
  if( !infd_isreg )
    return dec_stream( cfile_size, num_workers, infd, outfd, cl_opts, pp,
                       debug_level, in_slots, out_slots );

  const Lzip_index lzip_index( infd, cl_opts );
  if( lzip_index.retval() == 1 )	// decompress as stream if seek fails
    {
    lseek( infd, 0, SEEK_SET );
    return dec_stream( cfile_size, num_workers, infd, outfd, cl_opts, pp,
                       debug_level, in_slots, out_slots );
    }
  if( lzip_index.retval() != 0 )	// corrupt or invalid input file
    {
    if( lzip_index.good_magic() ) pp( lzip_index.error().c_str() );
    else show_file_error( pp.name(), lzip_index.error().c_str() );
    return lzip_index.retval();
    }
  const bool multi_empty = !from_stdin && lzip_index.multi_empty();

  if( num_workers > lzip_index.members() ) num_workers = lzip_index.members();

  if( outfd >= 0 )
    {
    struct stat st;
    if( !one_to_one || fstat( outfd, &st ) != 0 || !S_ISREG( st.st_mode ) ||
        lseek( outfd, 0, SEEK_CUR ) < 0 )
      {
      if( debug_level & 2 ) std::fputs( "decompress file to stdout.\n", stderr );
      if( verbosity >= 1 ) pp();
      show_progress( 0, cfile_size, &pp );			// init
      const int tmp = dec_stdout( num_workers, infd, outfd, pp, debug_level,
                                  out_slots, lzip_index );
      if( tmp ) return tmp;
      if( multi_empty ) { show_file_error( pp.name(), empty_msg ); return 2; }
      return 0;
      }
    }

  if( debug_level & 2 ) std::fputs( "decompress file to file.\n", stderr );
  if( verbosity >= 1 ) pp();
  show_progress( 0, cfile_size, &pp );			// init

  Worker_arg * worker_args = new( std::nothrow ) Worker_arg[num_workers];
  pthread_t * worker_threads = new( std::nothrow ) pthread_t[num_workers];
  if( !worker_args || !worker_threads )
    { pp( mem_msg ); delete[] worker_threads; delete[] worker_args; return 1; }

#if defined LZ_API_VERSION && LZ_API_VERSION >= 1012
  const bool nocopy = ( outfd < 0 && LZ_api_version() >= 1012 );
#else
  const bool nocopy = false;
#endif

  Shared_retval shared_retval;
  int i = 0;				// number of workers started
  for( ; i < num_workers; ++i )
    {
    worker_args[i].assign( lzip_index, pp, shared_retval, infd, num_workers,
                           outfd, i, nocopy );
    const int errcode =
      pthread_create( &worker_threads[i], 0, dworker, &worker_args[i] );
    if( errcode )
      { if( shared_retval.set_value( 1 ) )
          { show_error( "Can't create worker threads", errcode ); } break; }
    }

  while( --i >= 0 )
    {
    const int errcode = pthread_join( worker_threads[i], 0 );
    if( errcode && shared_retval.set_value( 1 ) )
      show_error( "Can't join worker threads", errcode );
    }
  delete[] worker_threads;
  delete[] worker_args;

  if( shared_retval() ) return shared_retval();	// some thread found a problem

  if( verbosity >= 1 )
    show_results( lzip_index.cdata_size(), lzip_index.udata_size(),
                  lzip_index.dictionary_size(), outfd < 0 );

  if( debug_level & 1 )
    std::fprintf( stderr,
      "workers started                           %8u\n", num_workers );

  if( multi_empty ) { show_file_error( pp.name(), empty_msg ); return 2; }
  return 0;
  }
