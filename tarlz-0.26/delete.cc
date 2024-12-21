/* Tarlz - Archiver with multimember lzip compression
   Copyright (C) 2013-2024 Antonio Diaz Diaz.

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

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <stdint.h>		// for lzlib.h
#include <unistd.h>
#include <lzlib.h>

#include "tarlz.h"
#include "arg_parser.h"
#include "lzip_index.h"
#include "archive_reader.h"


bool safe_seek( const int fd, const long long pos )
  {
  if( lseek( fd, pos, SEEK_SET ) == pos ) return true;
  show_error( seek_msg, errno ); return false;
  }


int tail_copy( const Arg_parser & parser, const Archive_descriptor & ad,
               std::vector< char > & name_pending, const long long istream_pos,
               const int outfd, int retval )
  {
  const long long rest = ad.lzip_index.file_size() - istream_pos;
  if( istream_pos > 0 && rest > 0 &&
      ( !safe_seek( ad.infd, istream_pos ) ||
        !copy_file( ad.infd, outfd, ad.namep, rest ) ) )
    { show_file_error( ad.namep, "Error during tail copy." );
      return retval ? retval : 1; }
  const long long ostream_pos = lseek( outfd, 0, SEEK_CUR );
  if( ostream_pos < 0 )
    { show_file_error( ad.namep, seek_msg, errno ); retval = 1; }
  else if( ostream_pos > 0 && ostream_pos < ad.lzip_index.file_size() )
    {
    int ret;
    do ret = ftruncate( outfd, ostream_pos );
      while( ret != 0 && errno == EINTR );
    if( ret != 0 || lseek( outfd, 0, SEEK_END ) != ostream_pos )
      {
      show_file_error( ad.namep, "Can't truncate archive", errno );
      if( retval < 1 ) retval = 1;
      }
    }

  if( ( close( outfd ) | close( ad.infd ) ) != 0 && retval == 0 )
    { show_file_error( ad.namep, eclosa_msg, errno ); retval = 1; }

  if( retval == 0 )
    for( int i = 0; i < parser.arguments(); ++i )
      if( nonempty_arg( parser, i ) && name_pending[i] )
        { show_file_error( parser.argument( i ).c_str(), nfound_msg );
          retval = 1; }
  return retval;
  }


/* Deleting from a corrupt archive must not worsen the corruption. Stop and
   tail-copy as soon as corruption is found.
*/
int delete_members( const Cl_options & cl_opts )
  {
  if( cl_opts.num_files <= 0 )
    { if( verbosity >= 1 ) show_error( "Nothing to delete." ); return 0; }
  if( cl_opts.archive_name.empty() )
    { show_error( "Deleting from stdin not implemented yet." ); return 1; }
  const Archive_descriptor ad( cl_opts.archive_name );
  if( ad.infd < 0 ) return 1;
  if( ad.name.size() && ad.indexed && ad.lzip_index.multi_empty() )
    { show_file_error( ad.namep, empty_msg ); close( ad.infd ); return 2; }
  const int outfd = open_outstream( cl_opts.archive_name, false );
  if( outfd < 0 ) { close( ad.infd ); return 1; }

  // mark member names to be deleted
  std::vector< char > name_pending( cl_opts.parser.arguments(), false );
  for( int i = 0; i < cl_opts.parser.arguments(); ++i )
    if( nonempty_arg( cl_opts.parser, i ) &&
        !Exclude::excluded( cl_opts.parser.argument( i ).c_str() ) )
      name_pending[i] = true;

  if( ad.indexed )		// archive is a compressed regular file
    return delete_members_lz( cl_opts, ad, name_pending, outfd );
  if( !ad.seekable )
    { show_file_error( ad.namep, "Archive is not seekable." ); return 1; }
  if( ad.lzip_index.file_size() < 3 * header_size )
    { show_file_error( ad.namep, has_lz_ext( ad.name ) ?
                       posix_lz_msg : posix_msg ); return 2; }
  // archive is uncompressed seekable, unless compressed corrupt

  Archive_reader ar( ad );		// serial reader
  Resizable_buffer rbuf;
  long long istream_pos = 0;		// source of next data move
  long long member_begin = 0;		// first pos of current tar member
  Extended extended;			// metadata from extended records
  int retval = 0;
  bool prev_extended = false;		// prev header was extended
  if( !rbuf.size() ) { show_error( mem_msg ); return 1; }

  while( true )				// process one tar header per iteration
    {
    if( !prev_extended && ( member_begin = lseek( ad.infd, 0, SEEK_CUR ) ) < 0 )
      { show_file_error( ad.namep, seek_msg, errno ); retval = 1; break; }
    Tar_header header;
    if( ( retval = ar.read( header, header_size ) ) != 0 )
      { show_file_error( ad.namep, ar.e_msg(), ar.e_code() ); break; }
    if( !check_ustar_chksum( header ) )		// error or EOA
      {
      if( block_is_zero( header, header_size ) )	// EOA
        {
        if( prev_extended && !cl_opts.permissive )
          { show_file_error( ad.namep, fv_msg1 ); retval = 2; }
        break;
        }
      // posix format already checked by archive reader
      show_file_error( ad.namep, bad_hdr_msg );
      retval = 2; break;
      }

    const Typeflag typeflag = (Typeflag)header[typeflag_o];
    if( typeflag == tf_global )
      {
      if( prev_extended && !cl_opts.permissive )
        { show_file_error( ad.namep, fv_msg2 ); retval = 2; break; }
      Extended dummy;		// global headers are parsed and ignored
      retval = ar.parse_records( dummy, header, rbuf, gblrec_msg, true );
      if( retval )
        { show_file_error( ad.namep, ar.e_msg(), ar.e_code() ); break; }
      continue;
      }
    if( typeflag == tf_extended )
      {
      if( prev_extended && !cl_opts.permissive )
        { show_file_error( ad.namep, fv_msg3 ); retval = 2; break; }
      if( ( retval = ar.parse_records( extended, header, rbuf, extrec_msg,
                                       cl_opts.permissive ) ) != 0 )
        { show_file_error( ad.namep, ar.e_msg(), ar.e_code() ); break; }
      if( !extended.crc_present() && cl_opts.missing_crc )
        { show_file_error( ad.namep, miscrc_msg ); retval = 2; break; }
      prev_extended = true; continue;
      }
    prev_extended = false;

    extended.fill_from_ustar( header );		// copy metadata from header

    if( ( retval = ar.skip_member( extended ) ) != 0 )
      { show_file_error( ad.namep, seek_msg, errno ); break; }

    // delete tar member
    if( !check_skip_filename( cl_opts, name_pending, extended.path().c_str() ) )
      {
      print_removed_prefix( extended.removed_prefix );
      if( !show_member_name( extended, header, 1, rbuf ) )
        { retval = 1; break; }
      const long long pos = lseek( ad.infd, 0, SEEK_CUR );
      if( pos <= 0 || pos <= member_begin || member_begin < istream_pos )
        { show_file_error( ad.namep, seek_msg, errno ); retval = 1; break; }
      const long long size = member_begin - istream_pos;
      if( size > 0 )	// move pending data each time a member is deleted
        {
        if( istream_pos == 0 )
          { if( !safe_seek( outfd, size ) ) { retval = 1; break; } }
        else if( !safe_seek( ad.infd, istream_pos ) ||
                 !copy_file( ad.infd, outfd, ad.namep, size ) ||
                 !safe_seek( ad.infd, pos ) ) { retval = 1; break; }
        }
      istream_pos = pos;
      }
    extended.reset();
    }

  return tail_copy( cl_opts.parser, ad, name_pending, istream_pos, outfd, retval );
  }
