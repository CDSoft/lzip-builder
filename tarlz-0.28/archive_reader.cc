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

#include <algorithm>
#include <cerrno>
#include <unistd.h>

#include "tarlz.h"
#include <lzlib.h>		// uint8_t defined in tarlz.h
#include "lzip_index.h"
#include "archive_reader.h"


namespace {

const char * const rdaerr_msg = "Error reading archive";

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

int non_tty_infd( const char * const name, const char * const namep )
  {
  int infd = name[0] ? open_instream( name ) : STDIN_FILENO;
  if( infd >= 0 && isatty( infd ) )		// for example /dev/tty
    { show_file_error( namep, name[0] ?
      "I won't read archive data from a terminal." :
      "I won't read archive data from a terminal (missing -f option?)" );
      close( infd ); infd = -1; }
  return infd;
  }


void xLZ_decompress_write( LZ_Decoder * const decoder,
                           const uint8_t * const buffer, const int size )
  {
  if( LZ_decompress_write( decoder, buffer, size ) != size )
    internal_error( "library error (LZ_decompress_write)." );
  }

} // end namespace


Archive_descriptor::Archive_descriptor( const std::string & archive_name )
  : name( archive_name ), namep( name.empty() ? "(stdin)" : name.c_str() ),
    infd( non_tty_infd( name.c_str(), namep ) ),
    lzip_index( infd ),
    seekable( lseek( infd, 0, SEEK_SET ) == 0 ),
    indexed( seekable && lzip_index.retval() == 0 ) {}


int Archive_reader_base::parse_records( Extended & extended,
                     const Tar_header header, Resizable_buffer & rbuf,
                     const char * const default_msg, const bool permissive,
                     std::vector< std::string > * const msg_vecp )
  {
  const long long edsize = parse_octal( header + size_o, size_l );
  const long long bufsize = round_up( edsize );
  if( edsize <= 0 ) return err( 2, misrec_msg );	// no extended records
  if( edsize >= 1LL << 33 || bufsize > extended.max_edata_size )
    return err( -2, longrec_msg );			// records too long
  if( !rbuf.resize( bufsize ) ) return err( -1, mem_msg );
  e_msg_ = ""; e_code_ = 0;
  int retval = read( rbuf.u8(), bufsize );	// extended records buffer
  if( retval == 0 && !extended.parse( rbuf(), edsize, permissive, msg_vecp ) )
    retval = 2;
  if( retval && !*e_msg_ ) e_msg_ = default_msg;
  return retval;
  }


/* Read 'size' uncompressed bytes, decompressing the input if needed.
   Return value: 0 = OK, 1 = OOM or read error, 2 = EOF or invalid data. */
int Archive_reader::read( uint8_t * const buf, const int size )
  {
  if( first_read )					// check format
    {
    first_read = false;
    uncompressed_seekable = ad.seekable && !ad.indexed &&
                            ad.lzip_index.file_size() > 3 * header_size;
    if( size != header_size )
      internal_error( "size != header_size on first call." );
    const int rd = readblock( ad.infd, buf, size );
    if( rd != size && errno ) return err( -1, rdaerr_msg, errno, rd );
    const Lzip_header & header = (*(const Lzip_header *)buf);
    const bool islz = rd >= min_member_size && header.check_magic() &&
                      header.check_version() &&
                      isvalid_ds( header.dictionary_size() );
    const bool istar = rd == size && check_ustar_chksum( buf );
    const bool iseoa =
      !islz && !istar && rd == size && block_is_zero( buf, size );
    bool maybe_lz = islz;			// maybe corrupt tar.lz
    if( !islz && !istar && !iseoa && rd > 0 )	// corrupt or invalid format
      {
      const bool lz_ext = has_lz_ext( ad.name );
      show_file_error( ad.namep, lz_ext ? posix_lz_msg : posix_msg );
      if( lz_ext && rd >= min_member_size ) maybe_lz = true;
      else if( rd == size ) return err( 2 );
      }
    if( !maybe_lz )					// uncompressed
      { if( rd == size ) return 0;
        return err( -2, "EOF reading archive.", 0, rd ); }
    uncompressed_seekable = false;			// compressed
    decoder = LZ_decompress_open();
    if( !decoder || LZ_decompress_errno( decoder ) != LZ_ok )
      { LZ_decompress_close( decoder ); decoder = 0; return err( -1, mem_msg ); }
    xLZ_decompress_write( decoder, buf, rd );
    const int ret = read( buf, size ); if( ret != 0 ) return ret;
    if( check_ustar_chksum( buf ) || block_is_zero( buf, size ) ) return 0;
    return err( 2, islz ? posix_lz_msg : "" );
    }

  if( !decoder )					// uncompressed
    {
    const int rd = readblock( ad.infd, buf, size );
    if( rd == size ) return 0; else return err( -2, end_msg, 0, rd );
    }
  const int ibuf_size = 16384;
  uint8_t ibuf[ibuf_size];
  int sz = 0;
  while( sz < size )
    {
    const int rd = LZ_decompress_read( decoder, buf + sz, size - sz );
    if( rd < 0 )
      {
      if( LZ_decompress_sync_to_member( decoder ) < 0 )
        internal_error( "library error (LZ_decompress_sync_to_member)." );
      e_skip_ = true; set_error_status( 2 ); return err( 2, "", 0, sz, true );
      }
    if( rd == 0 && LZ_decompress_finished( decoder ) == 1 )
      { return err( -2, end_msg, 0, sz ); }
    sz += rd;
    if( sz < size && !at_eof && LZ_decompress_write_size( decoder ) > 0 )
      {
      const int rsize = std::min( ibuf_size, LZ_decompress_write_size( decoder ) );
      const int rd = readblock( ad.infd, ibuf, rsize );
      xLZ_decompress_write( decoder, ibuf, rd );
      if( rd < rsize )
        {
        at_eof = true; LZ_decompress_finish( decoder );
        if( errno ) return err( -1, rdaerr_msg, errno, sz );
        }
      }
    }
  return 0;
  }


int Archive_reader::skip_member( const Extended & extended )
  {
  if( extended.file_size() <= 0 ) return 0;
  long long rest = round_up( extended.file_size() );	// size + padding
  if( uncompressed_seekable && lseek( ad.infd, rest, SEEK_CUR ) > 0 ) return 0;
  const int bufsize = 32 * header_size;
  uint8_t buf[bufsize];
  while( rest > 0 )				// skip tar member
    {
    const int rsize = ( rest >= bufsize ) ? bufsize : rest;
    const int ret = read( buf, rsize );
    if( ret != 0 ) return ret;
    rest -= rsize;
    }
  return 0;
  }


void Archive_reader_i::set_member( const long i )
  {
  LZ_decompress_reset( decoder );		// prepare for new member
  data_pos_ = ad.lzip_index.dblock( i ).pos();
  mdata_end_ = ad.lzip_index.dblock( i ).end();
  archive_pos = ad.lzip_index.mblock( i ).pos();
  member_id = i;
  }


/* Read 'size' decompressed bytes from the archive.
   Return value: 0 = OK, 1 = OOM or read error, 2 = EOF or invalid data. */
int Archive_reader_i::read( uint8_t * const buf, const int size )
  {
  int sz = 0;

  while( sz < size )
    {
    const int rd = LZ_decompress_read( decoder, buf + sz, size - sz );
    if( rd < 0 )
      return err( 2, LZ_strerror( LZ_decompress_errno( decoder ) ), 0, sz );
    if( rd == 0 && LZ_decompress_finished( decoder ) == 1 )
      return err( -2, end_msg, 0, sz );
    sz += rd; data_pos_ += rd;
    if( sz < size && LZ_decompress_write_size( decoder ) > 0 )
      {
      const long long ibuf_size = 16384;
      uint8_t ibuf[ibuf_size];
      const long long member_end = ad.lzip_index.mblock( member_id ).end();
      const long long rest = ( ( archive_pos < member_end ) ?
        member_end : ad.lzip_index.cdata_size() ) - archive_pos;
      const int rsize = std::min( LZ_decompress_write_size( decoder ),
                                  (int)std::min( ibuf_size, rest ) );
      if( rsize <= 0 ) LZ_decompress_finish( decoder );
      else
        {
        const int rd = preadblock( ad.infd, ibuf, rsize, archive_pos );
        xLZ_decompress_write( decoder, ibuf, rd );
        archive_pos += rd;
        if( rd < rsize )
          {
          LZ_decompress_finish( decoder );
          if( errno ) return err( -1, rdaerr_msg, errno, sz );
          }
        }
      }
    }
  return 0;
  }


int Archive_reader_i::skip_member( const Extended & extended )
  {
  if( extended.file_size() <= 0 ) return 0;
  long long rest = round_up( extended.file_size() );	// size + padding
  if( data_pos_ + rest == mdata_end_ ) { data_pos_ = mdata_end_; return 0; }
  const int bufsize = 32 * header_size;
  uint8_t buf[bufsize];
  while( rest > 0 )				// skip tar member
    {
    const int rsize = ( rest >= bufsize ) ? bufsize : rest;
    const int ret = read( buf, rsize );
    if( ret != 0 ) return ret;
    rest -= rsize;
    }
  return 0;
  }
