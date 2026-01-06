/* Tarlz - Archiver with multimember lzip compression
   Copyright (C) 2013-2026 Antonio Diaz Diaz.

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
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#if !defined __FreeBSD__ && !defined __OpenBSD__ && !defined __NetBSD__ && \
    !defined __DragonFly__ && !defined __APPLE__ && !defined __OS2__
#include <sys/sysmacros.h>	// major, minor
#else
#include <sys/types.h>		// major, minor
#endif
#include <ftw.h>
#include <grp.h>
#include <pwd.h>

#include "tarlz.h"
#include <lzlib.h>		// uint8_t defined in tarlz.h
#include "arg_parser.h"
#include "common_mutex.h"	// for fill_headers
#include "create.h"

#ifndef FTW_XDEV
#define FTW_XDEV FTW_MOUNT
#endif

Archive_attrs archive_attrs;	// archive attributes at time of creation


namespace {

const Cl_options * gcl_opts = 0;	// local vars needed by add_member
LZ_Encoder * encoder = 0;
const char * archive_namep = 0;
unsigned long long partial_data_size = 0;	// size of current block
Resizable_buffer grbuf;				// extended header + data
int goutfd = -1;


bool option_C_after_relative_filename_or_T( const Arg_parser & parser )
  {
  for( int i = 0; i < parser.arguments(); ++i )
    if( ( nonempty_arg( parser, i ) && parser.argument( i )[0] != '/' ) ||
        parser.code( i ) == 'T' )
      while( ++i < parser.arguments() )
        if( parser.code( i ) == 'C' ) return true;
  return false;
  }


/* Check archive type. Return position of EOA blocks or -1 if failure.
   If remove_eoa, leave fd file pos at beginning of the EOA blocks.
   Else, leave fd file pos at 0.
*/
long long check_compressed_appendable( const int fd, const bool remove_eoa )
  {
  struct stat st;				// fd must be regular
  if( fstat( fd, &st ) != 0 || !S_ISREG( st.st_mode ) ) return -1;
  if( lseek( fd, 0, SEEK_SET ) != 0 ) return -1;
  enum { bufsize = header_size + ( header_size / 8 ) };
  uint8_t buf[bufsize];
  const int rd = readblock( fd, buf, bufsize );
  if( rd == 0 && errno == 0 ) return 0;		// append to empty archive
  if( rd < min_member_size || ( rd != bufsize && errno ) ) return -1;
  const Lzip_header * const p = (const Lzip_header *)buf;	// shut up gcc
  if( !p->check_magic() || !p->check_version() ) return -1;
  LZ_Decoder * decoder = LZ_decompress_open();	// decompress first header
  if( !decoder || LZ_decompress_errno( decoder ) != LZ_ok ||
      LZ_decompress_write( decoder, buf, rd ) != rd ||
      LZ_decompress_read( decoder, buf, header_size ) != header_size )
    { LZ_decompress_close( decoder ); return -1; }
  LZ_decompress_close( decoder );
  const bool maybe_eoa = block_is_zero( buf, header_size );
  if( !check_ustar_chksum( buf ) && !maybe_eoa ) return -1;
  const long long end = lseek( fd, 0, SEEK_END );
  if( end < min_member_size ) return -1;

  Lzip_trailer trailer;				// read last trailer
  if( seek_read( fd, trailer.data, Lzip_trailer::size,
                 end - Lzip_trailer::size ) != Lzip_trailer::size ) return -1;
  const long long member_size = trailer.member_size();
  if( member_size < min_member_size || member_size > end ||
      ( maybe_eoa && member_size != end ) ) return -1;	// garbage after EOA?

  Lzip_header header;				// read last header
  if( seek_read( fd, header.data, Lzip_header::size,
                 end - member_size ) != Lzip_header::size ) return -1;
  if( !header.check_magic() || !header.check_version() ||
      !isvalid_ds( header.dictionary_size() ) ) return -1;

  // EOA marker in last member must contain between 512 and 32256 zeros alone
  const unsigned long long data_size = trailer.data_size();
  if( data_size < header_size || data_size > 32256 ) return -1;
  const unsigned data_crc = trailer.data_crc();
  const CRC32 crc32;
  uint32_t crc = 0xFFFFFFFFU;
  for( unsigned i = 0; i < data_size; ++i ) crc32.update_byte( crc, 0 );
  crc ^= 0xFFFFFFFFU;
  if( crc != data_crc ) return -1;

  const long long pos = remove_eoa ? end - member_size : 0;
  if( lseek( fd, pos, SEEK_SET ) != pos ) return -1;
  return end - member_size;
  }


/* Skip all tar headers.
   Return position of EOA blocks, -1 if failure, -2 if out of memory.
   If remove_eoa, leave fd file pos at beginning of the EOA blocks.
   Else, leave fd file pos at 0.
*/
long long check_uncompressed_appendable( const int fd, const bool remove_eoa )
  {
  struct stat st;			// fd must be regular
  if( fstat( fd, &st ) != 0 || !S_ISREG( st.st_mode ) ) return -1;
  if( lseek( fd, 0, SEEK_SET ) != 0 ) return -1;
  if( st.st_size <= 0 ) return 0;	// append to empty archive
  long long eoa_pos = 0;		// pos of EOA blocks
  Extended extended;			// metadata from extended records
  Resizable_buffer rbuf;		// extended records buffer
  bool prev_extended = false;		// prev header was extended
  if( !rbuf.size() ) return -2;

  while( true )				// process one tar header per iteration
    {
    Tar_header header;
    const int rd = readblock( fd, header, header_size );
    if( rd == 0 && errno == 0 ) break;		// missing EOA blocks
    if( rd != header_size ) return -1;
    if( !check_ustar_chksum( header ) )	// maybe EOA block
      { if( block_is_zero( header, header_size ) ) break; else return -1; }
    const Typeflag typeflag = (Typeflag)header[typeflag_o];
    if( typeflag == tf_extended || typeflag == tf_global )
      {
      if( prev_extended ) return -1;
      const long long edsize = parse_octal( header + size_o, size_l );
      const long long bufsize = round_up( edsize );
      if( bufsize <= 0 || bufsize > extended.max_edata_size ) return -1;
      if( !rbuf.resize( bufsize ) ) return -2;
      if( readblock( fd, rbuf.u8(), bufsize ) != bufsize )
        return -1;
      if( typeflag == tf_extended )
        { if( !extended.parse( rbuf(), edsize, false ) ) return -1;
          prev_extended = true; }
      continue;
      }
    prev_extended = false;

    eoa_pos = lseek( fd, round_up( extended.get_file_size_and_reset( header ) ),
                     SEEK_CUR );
    if( eoa_pos <= 0 ) return -1;
    }

  if( prev_extended ) return -1;
  const long long pos = remove_eoa ? eoa_pos : 0;
  if( lseek( fd, pos, SEEK_SET ) != pos ) return -1;
  return eoa_pos;
  }


bool archive_write( const uint8_t * const buf, const int size )
  {
  static bool flushed = true;		// avoid flushing empty lzip members

  if( size <= 0 && flushed ) return true;
  flushed = size <= 0;
  if( !encoder )					// uncompressed
    return writeblock_wrapper( goutfd, buf, size );
  enum { obuf_size = 65536 };
  uint8_t obuf[obuf_size];
  int sz = 0;
  if( size <= 0 ) LZ_compress_finish( encoder );	// flush encoder
  while( sz < size || size <= 0 )
    {
    const int wr = LZ_compress_write( encoder, buf + sz, size - sz );
    if( wr < 0 ) internal_error( "library error (LZ_compress_write)." );
    sz += wr;
    if( sz >= size && size > 0 ) break;		// minimize dictionary size
    const int rd = LZ_compress_read( encoder, obuf, obuf_size );
    if( rd < 0 ) internal_error( "library error (LZ_compress_read)." );
    if( rd == 0 && sz >= size ) break;
    if( !writeblock_wrapper( goutfd, obuf, rd ) ) return false;
    }
  if( LZ_compress_finished( encoder ) == 1 &&
      LZ_compress_restart_member( encoder, LLONG_MAX ) < 0 )
    internal_error( "library error (LZ_compress_restart_member)." );
  return true;
  }


// Return true if it stores filename in the ustar header.
bool store_name( const char * const filename, Extended & extended,
                 Tar_header header, const bool force_extended_name )
  {
  const char * const stored_name =
    remove_leading_dotslash( filename, &extended.removed_prefix, true );

  if( !force_extended_name )	// try storing filename in the ustar header
    {
    const int len = std::strlen( stored_name );
    enum { max_len = prefix_l + 1 + name_l };	// prefix + '/' + name
    if( len <= name_l )				// stored_name fits in name
      { std::memcpy( header + name_o, stored_name, len ); return true; }
    if( len <= max_len )			// find shortest prefix
      for( int i = len - name_l - 1; i < len && i <= prefix_l; ++i )
        if( stored_name[i] == '/' )		// stored_name can be split
          {
          std::memcpy( header + name_o, stored_name + i + 1, len - i - 1 );
          std::memcpy( header + prefix_o, stored_name, i );
          return true;
          }
    }
  // store filename in extended record, leave name zeroed in ustar header
  extended.path( stored_name );
  return false;
  }


// add one tar member to the archive and print filename
int add_member( const char * const filename, const struct stat *,
                const int flag, struct FTW * )
  {
  if( Exclude::excluded( filename ) ) return 0;		// skip excluded files
  long long file_size;
  Extended extended;		// metadata for extended records
  Tar_header header;
  std::string estr;
  if( !fill_headers( estr, filename, extended, header, file_size, flag ) )
    { if( estr.size() ) std::fputs( estr.c_str(), stderr ); return 0; }
  print_removed_prefix( extended.removed_prefix );
  const int infd = file_size ? open_instream( filename ) : -1;
  if( file_size && infd < 0 ) { set_error_status( 1 ); return 0; }

  const int ebsize = extended.format_block( grbuf );	// may be 0
  if( ebsize < 0 ) { show_error( extended.full_size_error() ); return 1; }
  if( encoder && gcl_opts->solidity == bsolid &&
      block_is_full( ebsize, file_size, gcl_opts->data_size,
                     partial_data_size ) && !archive_write( 0, 0 ) ) return 1;
  // write extended block to archive
  if( ebsize > 0 && !archive_write( grbuf.u8(), ebsize ) ) return 1;
  if( !archive_write( header, header_size ) ) return 1;

  if( file_size )
    {
    const long long bufsize = 32 * header_size;
    uint8_t buf[bufsize];
    long long rest = file_size;
    while( rest > 0 )
      {
      int size = std::min( rest, bufsize );
      const int rd = readblock( infd, buf, size );
      rest -= rd;
      if( rd != size )
        { show_atpos_error( filename, file_size - rest, false );
          close( infd ); return 1; }
      if( rest == 0 )				// last read
        {
        const int rem = file_size % header_size;
        if( rem > 0 )
          { const int padding = header_size - rem;
            std::memset( buf + size, 0, padding ); size += padding; }
        }
      if( !archive_write( buf, size ) ) { close( infd ); return 1; }
      }
    if( close( infd ) != 0 )
      { show_file_error( filename, eclosf_msg, errno ); return 1; }
    }
  if( encoder && gcl_opts->solidity == no_solid && !archive_write( 0, 0 ) )
    return 1;
  if( gcl_opts->warn_newer && archive_attrs.is_newer( filename ) )
    { show_file_error( filename, "File is newer than the archive." );
      set_error_status( 1 ); }
  if( verbosity >= 1 ) std::fprintf( stderr, "%s\n", filename );
  return 0;
  }


int call_nftw( const Cl_options & cl_opts, const char * const filename,
               const int flags,
               int (* add_memberp)( const char * const filename,
                   const struct stat *, const int flag, struct FTW * ) )
  {
  if( Exclude::excluded( filename ) ) return 0;	// skip excluded files
  struct stat st;
  if( lstat( filename, &st ) != 0 )
    { show_file_error( filename, cant_stat, errno ); set_error_status( 1 );
      return 0; }
  if( ( cl_opts.recursive && nftw( filename, add_memberp, 16, flags ) != 0 ) ||
      ( !cl_opts.recursive && add_memberp( filename, &st, 0, 0 ) != 0 ) )
    return 1;					// write error or OOM
  return 2;
  }


int read_t_list( const Cl_options & cl_opts, const char * const cl_filename,
                 const int flags,
                 int (* add_memberp)( const char * const filename,
                     const struct stat *, const int flag, struct FTW * ) )
  {
  const bool from_stdin = cl_filename[0] == '-' && cl_filename[1] == 0;
  const char * const filename = from_stdin ? "(stdin)" : cl_filename;
  FILE * f = from_stdin ? stdin : std::fopen( cl_filename, "r" );
  if( !f ) { show_file_error( filename, rd_open_msg, errno ); return 1; }
  enum { max_filename_size = 4096, bufsize = max_filename_size + 2 };
  char buf[bufsize];
  bool error = false;
  while( std::fgets( buf, bufsize, f ) )	// until error or EOF
    {
    int len = std::strlen( buf );
    if( len <= 0 || buf[len-1] != '\n' )
      { show_file_error( filename, ( len < bufsize - 1 ) ?
          "File name in list is unterminated or contains NUL bytes." :
          "File name too long in list." ); error = true; break; }
    do { buf[--len] = 0; }			// remove terminating newline
    while( len > 1 && buf[len-1] == '/' );	// and trailing slashes
    if( len <= 0 ) continue;			// empty name
    const int ret = call_nftw( cl_opts, buf, flags, add_memberp );
    if( ret == 0 ) continue;			// skip filename
    if( ret == 1 ) { error = true; break; }	// write error or OOM
    }
  if( error | std::ferror( f ) | !std::feof( f ) |
      ( f != stdin && std::fclose( f ) != 0 ) )
    { if( !error ) show_file_error( filename, rd_err_msg, errno ); return 1; }
  return 2;
  }


bool check_tty_out( const char * const archive_namep, const int outfd,
                    const bool to_stdout )
  {
  if( isatty( outfd ) )				// for example /dev/tty
    { show_file_error( archive_namep, to_stdout ?
      "I won't write archive data to a terminal (missing -f option?)" :
      "I won't write archive data to a terminal." );
      return false; }
  return true;
  }

} // end namespace


/* infd and outfd can refer to the same file if copying to a lower file
   position or if source and destination blocks don't overlap.
   max_size < 0 means no size limit. */
bool copy_file( const int infd, const int outfd, const char * const filename,
                const long long max_size )
  {
  const long long buffer_size = 65536;
  // remaining number of bytes to copy
  long long rest = (max_size >= 0) ? max_size : buffer_size;
  long long copied_size = 0;
  uint8_t * const buffer = new uint8_t[buffer_size];
  bool error = false;

  while( rest > 0 )
    {
    const int size = std::min( buffer_size, rest );
    if( max_size >= 0 ) rest -= size;
    const int rd = readblock( infd, buffer, size );
    if( rd != size && errno )
      { show_file_error( filename, rd_err_msg, errno ); error = true; break; }
    if( rd > 0 )
      {
      if( !writeblock_wrapper( outfd, buffer, rd ) ) { error = true; break; }
      copied_size += rd;
      }
    if( rd < size ) break;				// EOF
    }
  delete[] buffer;
  return !error && ( max_size < 0 || copied_size == max_size );
  }


bool writeblock_wrapper( const int outfd, const uint8_t * const buffer,
                         const int size )
  {
  if( writeblock( outfd, buffer, size ) != size )
    { show_file_error( archive_namep, wr_err_msg, errno ); return false; }
  return true;
  }


// write End-Of-Archive records
bool write_eoa_records( const int outfd, const bool compressed )
  {
  if( compressed )
    {
    enum { eoa_member_size = 44 };
    const uint8_t eoa_member[eoa_member_size] = {
      0x4C, 0x5A, 0x49, 0x50, 0x01, 0x0C, 0x00, 0x00, 0x6F, 0xFD, 0xFF, 0xFF,
      0xA3, 0xB7, 0x80, 0x0C, 0x82, 0xDB, 0xFF, 0xFF, 0x9F, 0xF0, 0x00, 0x00,
      0x2E, 0xAF, 0xB5, 0xEF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return writeblock_wrapper( outfd, eoa_member, eoa_member_size );
    }
  enum { bufsize = 2 * header_size };
  uint8_t buf[bufsize];
  std::memset( buf, 0, bufsize );
  return writeblock_wrapper( outfd, buf, bufsize );
  }


/* Remove any amount of leading "./" and '/' strings from filename.
   Optionally also remove prefixes containing a ".." component.
   Return the removed prefix in *removed_prefixp.
*/
const char * remove_leading_dotslash( const char * const filename,
                                      std::string * const removed_prefixp,
                                      const bool dotdot )
  {
  const char * p = filename;

  if( dotdot )
    for( int i = 0; filename[i]; ++i )
      if( dotdot_at_i( filename, i ) ) p = filename + i + 2;
  while( *p == '/' || ( *p == '.' && p[1] == '/' ) ) ++p;
  if( p != filename ) removed_prefixp->assign( filename, p - filename );
  else removed_prefixp->clear();		// no prefix was removed
  if( *p == 0 && *filename != 0 ) p = ".";
  return p;
  }


// set file_size != 0 only for regular files
bool fill_headers( std::string & estr, const char * const filename,
                   Extended & extended, Tar_header header,
                   long long & file_size, const int flag )
  {
  struct stat st;
  if( hstat( filename, &st, gcl_opts->dereference ) != 0 )
    { format_file_error( estr, filename, cant_stat, errno );
      set_error_status( 1 ); return false; }
  if( archive_attrs.is_the_archive( st ) )
    { format_file_error( estr, archive_namep,
        "Archive can't contain itself; not dumped." ); return false; }
  init_tar_header( header );
  bool force_extended_name = false;

  const mode_t mode = st.st_mode;
  print_octal( header + mode_o, mode_l - 1,
               mode & ( S_ISUID | S_ISGID | S_ISVTX |
                        S_IRWXU | S_IRWXG | S_IRWXO ) );
  const long long uid = ( gcl_opts->uid >= 0 ) ? gcl_opts->uid : st.st_uid;
  const long long gid = ( gcl_opts->gid >= 0 ) ? gcl_opts->gid : st.st_gid;
  if( uid_in_ustar_range( uid ) ) print_octal( header + uid_o, uid_l - 1, uid );
  else if( extended.set_uid( uid ) ) force_extended_name = true;
  if( uid_in_ustar_range( gid ) ) print_octal( header + gid_o, gid_l - 1, gid );
  else if( extended.set_gid( gid ) ) force_extended_name = true;
  const long long mtime = gcl_opts->mtime_set ? gcl_opts->mtime : st.st_mtime;
  if( time_in_ustar_range( mtime ) )
    print_octal( header + mtime_o, mtime_l - 1, mtime );
  else { extended.set_atime( gcl_opts->mtime_set ? mtime : st.st_atime );
         extended.set_mtime( mtime ); force_extended_name = true; }
  Typeflag typeflag;
  if( S_ISREG( mode ) ) typeflag = tf_regular;
  else if( S_ISDIR( mode ) )
    {
    typeflag = tf_directory;
    if( flag == FTW_DNR )
      { format_file_error( estr, filename, "Can't open directory", errno );
        set_error_status( 1 ); return false; }
    }
  else if( S_ISLNK( mode ) )
    {
    typeflag = tf_symlink;
    long len, sz;
    if( st.st_size <= linkname_l )
      {
      len = sz = readlink( filename, (char *)header + linkname_o, linkname_l );
      while( len > 1 && header[linkname_o+len-1] == '/' )	// trailing '/'
        { --len; header[linkname_o+len] = 0; }
      }
    else
      {
      char * const buf = new char[st.st_size+1];
      len = sz = readlink( filename, buf, st.st_size );
      if( sz == st.st_size )
        {
        while( len > 1 && buf[len-1] == '/' ) --len;	// trailing '/'
        if( len <= linkname_l ) std::memcpy( header + linkname_o, buf, len );
        else { buf[len] = 0; extended.linkpath( buf );
               force_extended_name = true; }
        }
      delete[] buf;
      }
    if( sz != st.st_size )
      {
      if( sz < 0 )
        format_file_error( estr, filename, "Error reading symbolic link", errno );
      else
        format_file_error( estr, filename, "Wrong size reading symbolic link.\n"
          "Please, send a bug report to the maintainers of your filesystem, "
          "mentioning\n'wrong st_size of symbolic link'.\nSee "
          "http://pubs.opengroup.org/onlinepubs/9799919799/basedefs/sys_stat.h.html" );
      set_error_status( 1 ); return false;
      }
    }
  else if( S_ISCHR( mode ) || S_ISBLK( mode ) )
    {
    typeflag = S_ISCHR( mode ) ? tf_chardev : tf_blockdev;
    if( (unsigned)major( st.st_rdev ) >= 2 << 20 ||
        (unsigned)minor( st.st_rdev ) >= 2 << 20 )
      { format_file_error( estr, filename,
                           "devmajor or devminor is larger than 2_097_151." );
        set_error_status( 1 ); return false; }
    print_octal( header + devmajor_o, devmajor_l - 1, major( st.st_rdev ) );
    print_octal( header + devminor_o, devminor_l - 1, minor( st.st_rdev ) );
    }
  else if( S_ISFIFO( mode ) ) typeflag = tf_fifo;
  else { format_file_error( estr, filename, "Unknown file type." );
         set_error_status( 2 ); return false; }
  header[typeflag_o] = typeflag;

  /* Get owner/group name if uid/gid is in range and names are not disabled.
     Prevent two threads from accessing a name database at the same time. */
  if( uid >= 0 && uid == (long long)( (uid_t)uid ) && !gcl_opts->numeric_owner )
    { static pthread_mutex_t uid_mutex = PTHREAD_MUTEX_INITIALIZER;
      static long long cached_uid = -1;
      static std::string cached_pw_name;
      xlock( &uid_mutex );
      if( uid != cached_uid )
        { const struct passwd * const pw = getpwuid( uid );
          if( !pw || !pw->pw_name || !pw->pw_name[0] ) goto no_uid;
          cached_uid = uid; cached_pw_name = pw->pw_name; }
      std::strncpy( (char *)header + uname_o, cached_pw_name.c_str(), uname_l - 1 );
no_uid: xunlock( &uid_mutex ); }
  if( gid >= 0 && gid == (long long)( (gid_t)gid ) && !gcl_opts->numeric_owner )
    { static pthread_mutex_t gid_mutex = PTHREAD_MUTEX_INITIALIZER;
      static long long cached_gid = -1;
      static std::string cached_gr_name;
      xlock( &gid_mutex );
      if( gid != cached_gid )
        { const struct group * const gr = getgrgid( gid );
          if( !gr || !gr->gr_name || !gr->gr_name[0] ) goto no_gid;
          cached_gid = gid; cached_gr_name = gr->gr_name; }
      std::strncpy( (char *)header + gname_o, cached_gr_name.c_str(), gname_l - 1 );
no_gid: xunlock( &gid_mutex ); }

  if( typeflag == tf_regular && st.st_size > extended.max_file_size )
    { format_file_error( estr, filename, large_file_msg );
      set_error_status( 1 ); return false; }
  file_size = ( typeflag == tf_regular && st.st_size > 0 ) ? st.st_size : 0;
  if( file_size >= 1LL << 33 )
    { extended.file_size( file_size ); force_extended_name = true; }
  else print_octal( header + size_o, size_l - 1, file_size );
  store_name( filename, extended, header, force_extended_name );
  print_octal( header + chksum_o, chksum_l - 1, ustar_chksum( header ) );
  return true;
  }


bool block_is_full( const int extended_size,
                    const unsigned long long file_size,
                    const unsigned long long target_size,
                    unsigned long long & partial_data_size )
  {
  const unsigned long long member_size =	// may overflow 'long long'
    extended_size + header_size + round_up( file_size );
  if( partial_data_size >= target_size ||
      ( partial_data_size >= min_data_size &&
        partial_data_size + member_size / 2 > target_size ) )
    { partial_data_size = member_size; return true; }
  partial_data_size += member_size; return false;
  }


unsigned ustar_chksum( const Tar_header header )
  {
  unsigned chksum = chksum_l * 0x20;	// treat chksum field as spaces
  for( int i = 0; i < chksum_o; ++i ) chksum += header[i];
  for( int i = chksum_o + chksum_l; i < header_size; ++i ) chksum += header[i];
  return chksum;
  }


bool check_ustar_chksum( const Tar_header header )
  { return check_ustar_magic( header ) &&
      ustar_chksum( header ) == parse_octal( header + chksum_o, chksum_l ); }


bool has_lz_ext( const std::string & name )
  {
  return ( name.size() > 3 &&
           name.compare( name.size() - 3, 3, ".lz" ) == 0 ) ||
         ( name.size() > 4 &&
           name.compare( name.size() - 4, 4, ".tlz" ) == 0 );
  }


int Cl_options::compressed() const	// tri-state bool with error (-2)
  {
  const int lz_ext = archive_name.empty() ? -1 : has_lz_ext( archive_name );
  if( !level_set ) return lz_ext;	// no level set in command line
  const bool cl_compressed = !uncompressed();
  if( lz_ext < 0 || lz_ext == cl_compressed ) return cl_compressed;
  show_file_error( archive_name.c_str(), lz_ext ?
                   "Uncompressed archive can't have .lz or .tlz extension." :
                   "Compressed archive requires .lz or .tlz extension." );
  return -2;
  }


int concatenate( const Cl_options & cl_opts )
  {
  if( cl_opts.num_files == 0 )
    { if( verbosity >= 1 ) show_error( "Nothing to concatenate." ); return 0; }
  int compressed = cl_opts.compressed();		// tri-state bool
  if( compressed == -2 ) return 1;
  const bool to_stdout = cl_opts.archive_name.empty();
  archive_namep = to_stdout ? "(stdout)" : cl_opts.archive_name.c_str();
  const int outfd =
    to_stdout ? STDOUT_FILENO : open_outstream( cl_opts.archive_name, false );
  if( outfd < 0 ) return 1;
  if( !check_tty_out( archive_namep, outfd, to_stdout ) )
    { close( outfd ); return 1; }
  if( !to_stdout && !archive_attrs.init( outfd ) )
    { show_file_error( archive_namep, "Can't stat", errno ); return 1; }
  if( !to_stdout && compressed >= 0 )	// level or ext are set in cl
    {
    const long long pos = compressed ?
                          check_compressed_appendable( outfd, true ) :
                          check_uncompressed_appendable( outfd, true );
    if( pos == -2 ) { show_error( mem_msg ); close( outfd ); return 1; }
    if( pos < 0 )
      { show_file_error( archive_namep, compressed ?
          "This does not look like an appendable tar.lz archive." :
          "This does not look like an appendable tar archive." );
        close( outfd ); return 2; }
    }

  int retval = 0;
  bool eoa_pending = false;
  for( int i = 0; i < cl_opts.parser.arguments(); ++i )	// copy archives
    {
    if( !nonempty_arg( cl_opts.parser, i ) ) continue;	// skip opts, empty names
    const char * const filename = cl_opts.parser.argument( i ).c_str();
    if( Exclude::excluded( filename ) ) continue;	// skip excluded files
    const int infd = open_instream( filename );
    if( infd < 0 ) { retval = 1; break; }
    struct stat st;
    if( !to_stdout && fstat( infd, &st ) == 0 &&
        archive_attrs.is_the_archive( st ) )
      { show_file_error( filename, "Archive can't contain itself; "
                         "not concatenated." ); close( infd ); continue; }
    long long size;
    if( compressed < 0 )		// not initialized yet
      {
      if( ( size = check_compressed_appendable( infd, false ) ) > 0 )
        compressed = true;
      else if( ( size = check_uncompressed_appendable( infd, false ) ) > 0 )
        compressed = false;
      else if( size != -2 ) { size = -1; compressed = has_lz_ext( filename ); }
      }
    else size = compressed ? check_compressed_appendable( infd, false ) :
                             check_uncompressed_appendable( infd, false );
    if( size == -2 )
      { show_error( mem_msg ); close( infd ); retval = 1; break; }
    if( size < 0 )
      { show_file_error( filename, compressed ?
                                   "Not an appendable tar.lz archive." :
                                   "Not an appendable tar archive." );
        close( infd ); retval = 2; break; }
    if( !copy_file( infd, outfd, filename, size ) || close( infd ) != 0 )
      { show_file_error( filename, "Error concatenating archive", errno );
        eoa_pending = false; retval = 1; break; }
    eoa_pending = true;
    if( verbosity >= 1 ) std::fprintf( stderr, "%s\n", filename );
    }

  if( eoa_pending && !write_eoa_records( outfd, compressed ) && retval == 0 )
    retval = 1;
  if( close( outfd ) != 0 && retval == 0 )
    { show_file_error( archive_namep, eclosa_msg, errno ); retval = 1; }
  return retval;
  }


// Return value: 0 = skip arg, 1 = error, 2 = arg done
int parse_cl_arg( const Cl_options & cl_opts, const int i,
                  int (* add_memberp)( const char * const filename,
                      const struct stat *, const int flag, struct FTW * ) )
  {
  const int code = cl_opts.parser.code( i );
  const std::string & arg = cl_opts.parser.argument( i );
  const char * filename = arg.c_str();	// filename from command line
  if( code == 'C' )
    { if( chdir( filename ) == 0 ) return 0;
      show_file_error( filename, chdir_msg, errno ); return 1; }
  if( code == 'T' || ( code == 0 && !arg.empty() ) )
    {
    const int flags = (cl_opts.depth ? FTW_DEPTH : 0) |
                      (cl_opts.dereference ? 0 : FTW_PHYS) |
                      (cl_opts.mount ? FTW_MOUNT : 0) |
                      (cl_opts.xdev ? FTW_XDEV : 0);
    if( code == 'T' )
      return read_t_list( cl_opts, filename, flags, add_memberp );
    std::string deslashed;		// filename without trailing slashes
    unsigned len = arg.size();
    while( len > 1 && arg[len-1] == '/' ) --len;
    if( len < arg.size() )		// remove trailing slashes
      { deslashed.assign( arg, 0, len ); filename = deslashed.c_str(); }
    return call_nftw( cl_opts, filename, flags, add_memberp );
    }
  return 0;				// skip options and empty names
  }


int encode( const Cl_options & cl_opts )
  {
  if( !grbuf.size() ) { show_error( mem_msg ); return 1; }
  int compressed = cl_opts.compressed();		// tri-state bool
  if( compressed == -2 ) return 1;
  const bool to_stdout = cl_opts.archive_name.empty();
  archive_namep = to_stdout ? "(stdout)" : cl_opts.archive_name.c_str();
  gcl_opts = &cl_opts;

  const bool append = cl_opts.program_mode == m_append;
  if( cl_opts.num_files == 0 && !cl_opts.option_T_present )
    {
    if( !append && !to_stdout )			// create archive
      { show_error( "Cowardly refusing to create an empty archive.", 0, true );
        return 1; }
    else		// create/append to stdout or append to archive
      { if( verbosity >= 1 ) show_error( "Nothing to append." ); return 0; }
    }

  if( to_stdout )				// create/append to stdout
    goutfd = STDOUT_FILENO;
  else						// create/append to archive
    if( ( goutfd = open_outstream( cl_opts.archive_name, !append ) ) < 0 )
      return 1;
  if( !check_tty_out( archive_namep, goutfd, to_stdout ) )
    { close( goutfd ); return 1; }
  if( append && !to_stdout )
    {
    long long pos;
    if( compressed < 0 )		// not initialized yet
      {
      if( ( pos = check_compressed_appendable( goutfd, true ) ) > 0 )
        compressed = true;
      else if( ( pos = check_uncompressed_appendable( goutfd, true ) ) > 0 )
        compressed = false;
      else if( pos != -2 ) { pos = -1; compressed = false; }	// unknown
      }
    else pos = compressed ? check_compressed_appendable( goutfd, true ) :
                            check_uncompressed_appendable( goutfd, true );
    if( pos == -2 ) { show_error( mem_msg ); close( goutfd ); return 1; }
    if( pos < 0 )
      { show_file_error( archive_namep, compressed ?
          "This does not look like an appendable tar.lz archive." :
          "This does not look like an appendable tar archive." );
        close( goutfd ); return 2; }
    }

  if( !archive_attrs.init( goutfd ) )
    { show_file_error( archive_namep, "Can't stat", errno );
      close( goutfd ); return 1; }

  if( !compressed )
    {
    /* CWD is not per-thread; multithreaded --create can't be used if an
       option -C appears in the command line after a relative filename or
       after an option -T. */
    if( cl_opts.parallel && cl_opts.num_workers > 1 &&
        ( !cl_opts.option_C_present ||
          !option_C_after_relative_filename_or_T( cl_opts.parser ) ) )
      {
      // show_file_error( archive_namep, "Multithreaded --create --un" );
      return encode_un( cl_opts, archive_namep, goutfd );
      }
    }
  else
    {
    if( cl_opts.solidity != asolid && cl_opts.solidity != solid &&
        cl_opts.num_workers > 0 && ( !cl_opts.option_C_present ||
        !option_C_after_relative_filename_or_T( cl_opts.parser ) ) )
      {
      // show_file_error( archive_namep, "Multithreaded --create" );
      return encode_lz( cl_opts, archive_namep, goutfd );
      }
    encoder = LZ_compress_open( option_mapping[cl_opts.level].dictionary_size,
                option_mapping[cl_opts.level].match_len_limit, LLONG_MAX );
    if( !encoder || LZ_compress_errno( encoder ) != LZ_ok )
      {
      if( !encoder || LZ_compress_errno( encoder ) == LZ_mem_error )
        show_error( mem_msg2 );
      else
        internal_error( "invalid argument to encoder." );
      close( goutfd ); return 1;
      }
    }

  int retval = 0;
  for( int i = 0; i < cl_opts.parser.arguments(); ++i )	// parse command line
    {
    const int ret = parse_cl_arg( cl_opts, i, add_member );
    if( ret == 0 ) continue;				// skip arg
    if( ret == 1 ) { retval = 1; break; }		// error
    if( encoder && cl_opts.solidity == dsolid &&	// end of group
        !archive_write( 0, 0 ) ) { retval = 1; break; }
    }

  if( retval == 0 )			// write End-Of-Archive records
    {
    enum { bufsize = 2 * header_size };
    uint8_t buf[bufsize];
    std::memset( buf, 0, bufsize );
    if( encoder &&
        ( cl_opts.solidity == asolid ||
          ( cl_opts.solidity == bsolid && partial_data_size ) ) &&
        !archive_write( 0, 0 ) ) retval = 1;		// flush encoder
    else if( !archive_write( buf, bufsize ) ||
             ( encoder && !archive_write( 0, 0 ) ) ) retval = 1;
    }
  if( encoder && LZ_compress_close( encoder ) < 0 )
    { show_error( "LZ_compress_close failed." ); retval = 1; }
  if( close( goutfd ) != 0 && retval == 0 )
    { show_file_error( archive_namep, eclosa_msg, errno ); retval = 1; }
  return final_exit_status( retval );
  }
