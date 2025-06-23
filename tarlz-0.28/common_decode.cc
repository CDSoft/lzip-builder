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

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>

#include "tarlz.h"
#include "arg_parser.h"
#include "decode.h"


namespace {

enum { mode_string_size = 10,
       group_string_size = 1 + uname_l + 1 + gname_l + 1 };	// 67

void format_mode_string( const Tar_header header, char buf[mode_string_size] )
  {
  const Typeflag typeflag = (Typeflag)header[typeflag_o];

  std::memcpy( buf, "----------", mode_string_size );
  switch( typeflag )
    {
    case tf_regular: break;
    case tf_link: buf[0] = 'h'; break;
    case tf_symlink: buf[0] = 'l'; break;
    case tf_chardev: buf[0] = 'c'; break;
    case tf_blockdev: buf[0] = 'b'; break;
    case tf_directory: buf[0] = 'd'; break;
    case tf_fifo: buf[0] = 'p'; break;
    case tf_hiperf: buf[0] = 'C'; break;
    default: buf[0] = '?';
    }
  const mode_t mode = parse_octal( header + mode_o, mode_l );	// 12 bits
  const bool setuid = mode & S_ISUID;
  const bool setgid = mode & S_ISGID;
  const bool sticky = mode & S_ISVTX;
  if( mode & S_IRUSR ) buf[1] = 'r';
  if( mode & S_IWUSR ) buf[2] = 'w';
  if( mode & S_IXUSR ) buf[3] = setuid ? 's' : 'x';
  else if( setuid ) buf[3] = 'S';
  if( mode & S_IRGRP ) buf[4] = 'r';
  if( mode & S_IWGRP ) buf[5] = 'w';
  if( mode & S_IXGRP ) buf[6] = setgid ? 's' : 'x';
  else if( setgid ) buf[6] = 'S';
  if( mode & S_IROTH ) buf[7] = 'r';
  if( mode & S_IWOTH ) buf[8] = 'w';
  if( mode & S_IXOTH ) buf[9] = sticky ? 't' : 'x';
  else if( sticky ) buf[9] = 'T';
  }


int format_user_group_string( const Extended & extended,
                              const Tar_header header,
                              char buf[group_string_size] )
  {
  int len;
  if( header[uname_o] && header[gname_o] )
    len = snprintf( buf, group_string_size,
                    " %.32s/%.32s", header + uname_o, header + gname_o );
  else
    len = snprintf( buf, group_string_size, " %llu/%llu",
                    extended.get_uid(), extended.get_gid() );
  return len;
  }


// return true if dir is a parent directory of name
bool compare_prefix_dir( const char * const dir, const char * const name )
  {
  int len = 0;
  while( dir[len] && dir[len] == name[len] ) ++len;
  return !dir[len] && len > 0 && ( dir[len-1] == '/' || name[len] == '/' );
  }


// compare two file names ignoring trailing slashes
bool compare_tslash( const char * const name1, const char * const name2 )
  {
  const char * p = name1;
  const char * q = name2;
  while( *p && *p == *q ) { ++p; ++q; }
  while( *p == '/' ) ++p;
  while( *q == '/' ) ++q;
  return !*p && !*q;
  }


/* Return the address of a malloc'd buffer containing the file data and
   the file size in '*file_sizep'.
   In case of error, return 0 and do not modify '*file_sizep'.
*/
char * read_file( const char * const cl_filename, long * const file_sizep )
  {
  const char * const large_file4_msg = "File is larger than 4 GiB.";
  const bool from_stdin = cl_filename[0] == '-' && cl_filename[1] == 0;
  const char * const filename = from_stdin ? "(stdin)" : cl_filename;
  struct stat in_stats;
  const int infd =
    from_stdin ? STDIN_FILENO : open_instream( filename, &in_stats );
  if( infd < 0 ) return 0;
  const long long max_size = 1LL << 32;
  long long buffer_size = ( !from_stdin && S_ISREG( in_stats.st_mode ) ) ?
    in_stats.st_size + 1 : 65536;
  if( buffer_size > max_size + 1 )
    { show_file_error( filename, large_file4_msg ); close( infd ); return 0; }
  if( buffer_size >= LONG_MAX )
    { show_file_error( filename, large_file_msg ); close( infd ); return 0; }
  uint8_t * buffer = (uint8_t *)std::malloc( buffer_size );
  if( !buffer )
    { show_file_error( filename, mem_msg ); close( infd ); return 0; }
  long long file_size = readblock( infd, buffer, buffer_size );
  while( file_size >= buffer_size && file_size < max_size && !errno )
    {
    if( buffer_size >= LONG_MAX )
      { show_file_error( filename, large_file_msg ); std::free( buffer );
        close( infd ); return 0; }
    buffer_size = (buffer_size <= LONG_MAX / 2) ? 2 * buffer_size : LONG_MAX;
    uint8_t * const tmp = (uint8_t *)std::realloc( buffer, buffer_size );
    if( !tmp )
      { show_file_error( filename, mem_msg ); std::free( buffer );
        close( infd ); return 0; }
    buffer = tmp;
    file_size += readblock( infd, buffer + file_size, buffer_size - file_size );
    }
  if( errno )
    { show_file_error( filename, rd_err_msg, errno );
      std::free( buffer ); close( infd ); return 0; }
  if( close( infd ) != 0 )
    { show_file_error( filename, "Error closing input file", errno );
      std::free( buffer ); return 0; }
  if( file_size > max_size )
    { show_file_error( filename, large_file4_msg );
      std::free( buffer ); return 0; }
  if( file_size + 1 < buffer_size )
    {
    uint8_t * const tmp =
      (uint8_t *)std::realloc( buffer, std::max( 1LL, file_size ) );
    if( !tmp )
      { show_file_error( filename, mem_msg ); std::free( buffer );
        close( infd ); return 0; }
    buffer = tmp;
    }
  *file_sizep = file_size;
  return (char *)buffer;
  }

} // end namespace


bool block_is_zero( const uint8_t * const buf, const int size )
  {
  for( int i = 0; i < size; ++i ) if( buf[i] != 0 ) return false;
  return true;
  }


bool format_member_name( const Extended & extended, const Tar_header header,
                         Resizable_buffer & rbuf, const bool long_format )
  {
  if( !long_format )
    {
    if( !rbuf.resize( extended.path().size() + 2 ) ) return false;
    snprintf( rbuf(), rbuf.size(), "%s\n", extended.path().c_str() );
    return true;
    }
  format_mode_string( header, rbuf() );
  const int group_string_len =
    format_user_group_string( extended, header, rbuf() + mode_string_size );
  int offset = mode_string_size + group_string_len;
  const time_t mtime = extended.mtime().sec();
  struct tm t;
  char buf[32];	// if local time and UTC fail, use seconds since epoch
  if( localtime_r( &mtime, &t ) || gmtime_r( &mtime, &t ) )
    snprintf( buf, sizeof buf, "%04d-%02u-%02u %02u:%02u", 1900 + t.tm_year,
              1 + t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min );
  else snprintf( buf, sizeof buf, "%lld", extended.mtime().sec() );
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  const bool islink = typeflag == tf_link || typeflag == tf_symlink;
  const char * const link_string = !islink ? "" :
                       ( ( typeflag == tf_link ) ? " link to " : " -> " );
  // print "user/group size" in a field of width 19 with 8 or more for size
  if( typeflag == tf_chardev || typeflag == tf_blockdev )
    {
    const unsigned devmajor = parse_octal( header + devmajor_o, devmajor_l );
    const unsigned devminor = parse_octal( header + devminor_o, devminor_l );
    const int width = std::max( 1,
      std::max( 8, 19 - group_string_len ) - 1 - decimal_digits( devminor ) );
    offset += snprintf( rbuf() + offset, rbuf.size() - offset, " %*u,%u",
                        width, devmajor, devminor );
    }
  else
    {
    const int width = std::max( 8, 19 - group_string_len );
    offset += snprintf( rbuf() + offset, rbuf.size() - offset, " %*llu",
                        width, extended.file_size() );
    }
  for( int i = 0; i < 2; ++i )	// resize rbuf if not large enough
    {
    const int len = snprintf( rbuf() + offset, rbuf.size() - offset,
              " %s %s%s%s\n", buf, extended.path().c_str(), link_string,
              islink ? extended.linkpath().c_str() : "" );
    if( len + offset < (int)rbuf.size() ) { offset += len; break; }
    if( !rbuf.resize( len + offset + 1 ) ) return false;
    }
  if( rbuf()[0] == '?' )
    {
    if( !rbuf.resize( offset + 25 + 1 ) ) return false;
    snprintf( rbuf() + offset - 1, rbuf.size() - offset,
              ": Unknown file type 0x%02X\n", typeflag );
    }
  return true;
  }


bool show_member_name( const Extended & extended, const Tar_header header,
                       const int vlevel, Resizable_buffer & rbuf )
  {
  if( verbosity >= vlevel )
    {
    if( !format_member_name( extended, header, rbuf, verbosity > vlevel ) )
      { show_error( mem_msg ); return false; }
    std::fputs( rbuf(), stdout );
    std::fflush( stdout );
    }
  return true;
  }


/* Return true if file must be skipped.
   Execute -C options if cwd_fd >= 0 (diff or extract).
   Each name specified in the command line or in the argument to option -T
   matches all members with the same name in the archive. */
bool check_skip_filename( const Cl_options & cl_opts, Cl_names & cl_names,
                          const char * const filename, const int cwd_fd,
                          std::string * const msgp )
  {
  static int c_idx = -1;		// parser index of last -C executed
  if( Exclude::excluded( filename ) ) return true;	// skip excluded files
  if( cl_opts.num_files <= 0 && !cl_opts.option_T_present ) return false;
  bool skip = true;	// else skip all but the files (or trees) specified
  bool chdir_pending = false;

  const Arg_parser & parser = cl_opts.parser;
  for( int i = 0; i < parser.arguments(); ++i )
    {
    if( parser.code( i ) == 'C' ) { chdir_pending = true; continue; }
    if( !nonempty_arg( parser, i ) && parser.code( i ) != 'T' ) continue;
    std::string removed_prefix;			// prefix of cl argument
    bool match = false;
    if( parser.code( i ) == 'T' )
      {
      T_names & t_names = cl_names.t_names( i );
      for( unsigned j = 0; j < t_names.names(); ++j )
        {
        const char * const name =
          remove_leading_dotslash( t_names.name( j ), &removed_prefix );
        if( ( cl_opts.recursive && compare_prefix_dir( name, filename ) ) ||
            compare_tslash( name, filename ) )
          { match = true; t_names.reset_name_pending( j ); break; }
        }
      }
    else
      {
      const char * const name =
        remove_leading_dotslash( parser.argument( i ).c_str(), &removed_prefix );
      if( ( cl_opts.recursive && compare_prefix_dir( name, filename ) ) ||
          compare_tslash( name, filename ) )
        { match = true; cl_names.name_pending_or_idx[i] = false; }
      }
    if( match )
      {
      print_removed_prefix( removed_prefix, msgp );
      skip = false;
      // only serial decoder sets cwd_fd >= 0 to process -C options
      if( chdir_pending && cwd_fd >= 0 )
        {
        if( c_idx > i )
          { if( fchdir( cwd_fd ) != 0 )
            { show_error( "Error changing to initial working directory", errno );
              throw Chdir_error(); } c_idx = -1; }
        for( int j = c_idx + 1; j < i; ++j )
          {
          if( parser.code( j ) != 'C' ) continue;
          const char * const dir = parser.argument( j ).c_str();
          if( chdir( dir ) != 0 )
            { show_file_error( dir, chdir_msg, errno ); throw Chdir_error(); }
          c_idx = j;
          }
        }
      break;
      }
    }
  return skip;
  }


bool make_dirs( const std::string & name )
  {
  int i = name.size();
  while( i > 0 && name[i-1] == '/' ) --i;	// remove trailing slashes
  while( i > 0 && name[i-1] != '/' ) --i;	// remove last component
  while( i > 0 && name[i-1] == '/' ) --i;	// remove more slashes
  const int dirsize = i;		// first slash before last component
  struct stat st;

  if( dirsize > 0 && lstat( std::string( name, 0, dirsize ).c_str(), &st ) == 0 )
    { if( !S_ISDIR( st.st_mode ) ) { errno = ENOTDIR; return false; }
      return true; }
  for( i = 0; i < dirsize; )	// if dirsize == 0, dirname is '/' or empty
    {
    while( i < dirsize && name[i] == '/' ) ++i;
    const int first = i;
    while( i < dirsize && name[i] != '/' ) ++i;
    if( first < i )
      {
      const std::string partial( name, 0, i );
      const mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      if( lstat( partial.c_str(), &st ) == 0 )
        { if( !S_ISDIR( st.st_mode ) ) { errno = ENOTDIR; return false; } }
      else if( mkdir( partial.c_str(), mode ) != 0 && errno != EEXIST )
        return false;	// if EEXIST, another thread or process created the dir
      }
    }
  return true;
  }


T_names::T_names( const char * const filename )
  {
  buffer = read_file( filename, &file_size );
  if( !buffer ) std::exit( 1 );
  for( long i = 0; i < file_size; )
    {
    char * const p = (char *)std::memchr( buffer + i, '\n', file_size - i );
    if( !p ) { show_file_error( filename, "Unterminated file name in list." );
               std::exit( 1 ); }
    *p = 0;				// overwrite newline terminator
    const long idx = p - buffer;
    if( idx - i > 4096 )
      { show_file_error( filename, "File name too long in list." );
        std::exit( 1 ); }
    if( idx - i > 0 ) { name_idx.push_back( i ); } i = idx + 1;
    }
  name_pending_.resize( name_idx.size(), true );
  }


Cl_names::Cl_names( const Arg_parser & parser )
  : name_pending_or_idx( parser.arguments(), false )
  {
  for( int i = 0; i < parser.arguments(); ++i )
    {
    if( parser.code( i ) == 'T' )
      {
      if( t_vec.size() >= 256 )
        { show_file_error( parser.argument( i ).c_str(),
            "More than 256 '-T' options in command line." ); std::exit( 1 ); }
      name_pending_or_idx[i] = t_vec.size();
      t_vec.push_back( new T_names( parser.argument( i ).c_str() ) );
      }
    else if( nonempty_arg( parser, i ) ) name_pending_or_idx[i] = true;
    }
  }


bool Cl_names::names_remain( const Arg_parser & parser ) const
  {
  bool not_found = false;
  for( int i = 0; i < parser.arguments(); ++i )
    {
    if( parser.code( i ) == 'T' )
      {
      const T_names & t_names = *t_vec[name_pending_or_idx[i]];
      for( unsigned j = 0; j < t_names.names(); ++j )
        if( t_names.name_pending( j ) &&
          !Exclude::excluded( t_names.name( j ) ) )
          { show_file_error( t_names.name( j ), nfound_msg );
            not_found = true; }
      }
    else if( nonempty_arg( parser, i ) && name_pending_or_idx[i] &&
        !Exclude::excluded( parser.argument( i ).c_str() ) )
      { show_file_error( parser.argument( i ).c_str(), nfound_msg );
        not_found = true; }
    }
  return not_found;
  }
