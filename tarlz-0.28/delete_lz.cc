/* Tarlz - Archiver with multimember lzip compression
   Copyright (C) 2013-2025 Antonio Diaz Diaz.

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
#include <unistd.h>

#include "tarlz.h"
#include <lzlib.h>		// uint8_t defined in tarlz.h
#include "arg_parser.h"
#include "lzip_index.h"
#include "archive_reader.h"
#include "decode.h"


/* Deleting from a corrupt archive must not worsen the corruption. Stop and
   tail-copy as soon as corruption is found.
*/
int delete_members_lz( const Cl_options & cl_opts,
                       const Archive_descriptor & ad,
                       Cl_names & cl_names, const int outfd )
  {
  Archive_reader_i ar( ad );		// indexed reader
  Resizable_buffer rbuf;
  if( !rbuf.size() || ar.fatal() ) { show_error( mem_msg ); return 1; }

  long long istream_pos = 0;		// source of next data move
  int retval = 0, retval2 = 0;
  for( long i = 0; i < ad.lzip_index.members(); ++i )
    {
    if( ad.lzip_index.dblock( i ).size() == 0 ) continue;  // empty lzip member
    long long member_begin = 0;		// first pos of current tar member
    Extended extended;			// metadata from extended records
    bool prev_extended = false;		// prev header was extended
    ar.set_member( i );			// prepare for new member
    while( true )			// process one tar header per iteration
      {
      if( ar.data_pos() >= ar.mdata_end() )
        {
        if( ar.at_member_end() && !prev_extended ) break;
        // member end exceeded or ends in extended
        show_file_error( ad.namep, "Member misalignment found." );
        retval = 2; goto done;
        }
      if( !prev_extended ) member_begin = ar.data_pos();
      Tar_header header;
      if( ( retval = ar.read( header, header_size ) ) != 0 )
        { show_file_error( ad.namep, ar.e_msg(), ar.e_code() ); goto done; }
      if( !check_ustar_chksum( header ) )		// error or EOA
        {
        if( block_is_zero( header, header_size ) )	// EOA
          {
          if( prev_extended && !cl_opts.permissive )
            { show_file_error( ad.namep, fv_msg1 ); retval = 2; }
          goto done;
          }
        // indexed archive reader does not check posix format
        show_file_error( ad.namep, ( ar.data_pos() > header_size ) ?
                         bad_hdr_msg : posix_lz_msg );
        retval = 2;
        goto done;
        }

      const Typeflag typeflag = (Typeflag)header[typeflag_o];
      if( typeflag == tf_global )
        {
        if( prev_extended && !cl_opts.permissive )
          { show_file_error( ad.namep, fv_msg2 ); retval = 2; goto done; }
        Extended dummy;		// global headers are parsed and ignored
        retval = ar.parse_records( dummy, header, rbuf, gblrec_msg, true );
        if( retval )
          { show_file_error( ad.namep, ar.e_msg(), ar.e_code() ); goto done; }
        continue;
        }
      if( typeflag == tf_extended )
        {
        if( prev_extended && !cl_opts.permissive )
          { show_file_error( ad.namep, fv_msg3 ); retval = 2; goto done; }
        if( ( retval = ar.parse_records( extended, header, rbuf, extrec_msg,
                                         cl_opts.permissive ) ) != 0 )
          { show_file_error( ad.namep, ar.e_msg(), ar.e_code() ); goto done; }
        if( !extended.crc_present() && cl_opts.missing_crc )
          { show_file_error( ad.namep, miscrc_msg ); retval = 2; goto done; }
        prev_extended = true; continue;
        }
      prev_extended = false;

      extended.fill_from_ustar( header );	// copy metadata from header

      if( ( retval = ar.skip_member( extended ) ) != 0 ) goto done;

      // delete tar member
      if( !check_skip_filename( cl_opts, cl_names, extended.path().c_str() ) )
        {
        print_removed_prefix( extended.removed_prefix );
        // check that members match
        if( member_begin != ad.lzip_index.dblock( i ).pos() || !ar.at_member_end() )
          { show_file_error( extended.path().c_str(),
                             "Can't delete: not compressed individually." );
            retval2 = 2; extended.reset(); continue; }
        if( !show_member_name( extended, header, 1, rbuf ) )
          { retval = 1; goto done; }
        const long long size = ad.lzip_index.mblock( i ).pos() - istream_pos;
        if( size > 0 )	// move pending data each time a member is deleted
          {
          if( istream_pos == 0 )
            { if( !safe_seek( outfd, size ) ) { retval = 1; goto done; } }
          else if( !safe_seek( ad.infd, istream_pos ) ||
                   !copy_file( ad.infd, outfd, ad.namep, size ) )
            { retval = 1; goto done; }
          }
        istream_pos = ad.lzip_index.mblock( i ).end();	// member end
        }
      extended.reset();
      }
    }
done:
  if( retval < retval2 ) retval = retval2;
  // tail copy keeps trailing data
  return tail_copy( cl_opts.parser, ad, cl_names, istream_pos, outfd, retval );
  }
