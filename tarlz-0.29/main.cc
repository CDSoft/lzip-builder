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
/*
   Exit status: 0 for a normal exit, 1 for environmental problems
   (file not found, files differ, invalid command-line options, I/O errors,
   etc), 2 to indicate a corrupt or invalid input file, 3 for an internal
   consistency error (e.g., bug) which caused tarlz to panic.
*/

#define _FILE_OFFSET_BITS 64

#include <cctype>
#include <cerrno>
#include <climits>		// CHAR_BIT, SSIZE_MAX
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>		// pthread_t
#include <unistd.h>
#include <sys/stat.h>
#include <grp.h>
#include <pwd.h>
#if defined __OS2__
#include <io.h>
#endif

#include "tarlz.h"		// SIZE_MAX
#include <lzlib.h>		// uint8_t defined in tarlz.h
#include "arg_parser.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#if CHAR_BIT != 8
#error "Environments where CHAR_BIT != 8 are not supported."
#endif

#if ( defined  SIZE_MAX &&  SIZE_MAX < ULONG_MAX ) || \
    ( defined SSIZE_MAX && SSIZE_MAX <  LONG_MAX )
#error "Environments where 'size_t' is narrower than 'long' are not supported."
#endif

int verbosity = 0;
const char * const program_name = "tarlz";

namespace {

const char * const program_year = "2026";
const char * invocation_name = program_name;		// default value


void show_help( const long num_online )
  {
  std::printf(
    "Tarlz is a massively parallel (multithreaded) combined implementation of the\n"
    "tar archiver and the lzip compressor. Tarlz uses the compression library\n"
    "lzlib.\n"
    "\nTarlz creates tar archives using a simplified and safer variant of the POSIX\n"
    "pax format compressed in lzip format, keeping the alignment between tar\n"
    "members and lzip members. The resulting multimember tar.lz archive is\n"
    "backward compatible with standard tar tools like GNU tar, which treat it\n"
    "like any other tar.lz archive. Tarlz can append files to the end of such\n"
    "compressed archives.\n"
    "\nKeeping the alignment between tar members and lzip members has two\n"
    "advantages. It adds an indexed lzip layer on top of the tar archive, making\n"
    "it possible to decode the archive safely in parallel. It also reduces the\n"
    "amount of data lost in case of corruption.\n"
    "\nThe tarlz file format is a safe POSIX-style backup format. In case of\n"
    "corruption, tarlz can extract all the undamaged members from the tar.lz\n"
    "archive, skipping over the damaged members, just like the standard\n"
    "(uncompressed) tar. Moreover, the option '--keep-damaged' can be used to\n"
    "recover as much data as possible from each damaged member, and lziprecover\n"
    "can be used to recover some of the damaged members.\n"
    "\nUsage: %s operation [options] [files]\n", invocation_name );
  std::printf( "\nOperations:\n"
    "  -?, --help                  display this help and exit\n"
    "  -V, --version               output version information and exit\n"
    "  -A, --concatenate           append archives to the end of an archive\n"
    "  -c, --create                create a new archive\n"
    "  -d, --diff                  find differences between archive and file system\n"
    "      --delete                delete files/directories from an archive\n"
    "  -r, --append                append files to the end of an archive\n"
    "  -t, --list                  list the contents of an archive\n"
    "  -x, --extract               extract files/directories from an archive\n"
    "  -z, --compress              compress existing POSIX tar archives\n"
    "      --check-lib             check version of lzlib and exit\n"
    "      --time-bits             print the size of time_t in bits and exit\n"
    "\nOptions:\n"
    "  -B, --data-size=<bytes>     set target size of input data blocks [2x8=16 MiB]\n"
    "  -C, --directory=<dir>       change to directory <dir>\n"
    "  -f, --file=<archive>        use archive file <archive>\n"
    "  -h, --dereference           follow symlinks; archive the files they point to\n"
    "  -n, --threads=<n>           set number of (de)compression threads [%ld]\n"
    "  -o, --output=<file>         compress to <file> ('-' for stdout)\n"
    "  -p, --preserve-permissions  don't subtract the umask on extraction\n"
    "  -q, --quiet                 suppress all messages\n"
    "  -R, --no-recursive          don't operate recursively on directories\n"
    "      --recursive             operate recursively on directories (default)\n"
    "  -T, --files-from=<file>     get file names from <file>\n"
    "  -v, --verbose               verbosely list files processed\n"
    "  -0 .. -9                    set compression level [default 6]\n"
    "      --uncompressed          create an uncompressed archive\n"
    "        --asolid              create solidly compressed appendable archive\n"
    "        --bsolid              create per block compressed archive (default)\n"
    "        --dsolid              create per directory compressed archive\n"
    "        --no-solid            create per file compressed archive\n"
    "        --solid               create solidly compressed archive\n"
    "      --anonymous             equivalent to '--owner=root --group=root'\n"
    "        --owner=<owner>       use <owner> name/ID for files added to archive\n"
    "        --group=<group>       use <group> name/ID for files added to archive\n"
    "        --numeric-owner       don't write owner or group names to archive\n"
    "      --depth                 archive entries before the directory itself\n"
    "      --exclude=<pattern>     exclude files matching a shell pattern\n"
    "      --ignore-ids            ignore differences in owner and group IDs\n"
    "      --ignore-metadata       compare only file size and file content\n"
    "      --ignore-overflow       ignore mtime overflow differences on 32-bit\n"
    "      --keep-damaged          don't delete partially extracted files\n"
    "      --missing-crc           exit with error status if missing extended CRC\n"
    "      --mount, --xdev         stay in local file system when creating archive\n"
    "      --mtime=<date>          use <date> as mtime for files added to archive\n"
    "      --out-slots=<n>         number of 1 MiB output packets buffered [64]\n"
    "      --parallel              create uncompressed archive in parallel\n"
    "      --warn-newer            warn if any file is newer than the archive\n",
    num_online );
  if( verbosity >= 1 ) std::fputs(
    "      --debug=<level>         (0-1) print debug statistics to stderr\n", stdout );
  std::fputs(
    "\nIf no archive is specified, tarlz tries to read it from standard input or\n"
    "write it to standard output.\n"
    "Numbers may contain underscore separators between groups of digits and\n"
    "may be followed by a SI or binary multiplier: 1_234_567kB, 4KiB.\n"
    "\n*Exit status*\n"
    "0 for a normal exit, 1 for environmental problems (file not found, files\n"
    "differ, invalid command-line options, I/O errors, etc), 2 to indicate a\n"
    "corrupt or invalid input file, 3 for an internal consistency error (e.g.,\n"
    "bug) which caused tarlz to panic.\n"
    "\nReport bugs to lzip-bug@nongnu.org\n"
    "Tarlz home page: http://www.nongnu.org/lzip/tarlz.html\n", stdout );
  }


void show_lzlib_version()
  {
  std::printf( "Using lzlib %s\n", LZ_version() );
#if !defined LZ_API_VERSION
  std::fputs( "LZ_API_VERSION is not defined.\n", stdout );
#elif LZ_API_VERSION >= 1012
  std::printf( "Using LZ_API_VERSION = %u\n", LZ_api_version() );
#else
  std::printf( "Compiled with LZ_API_VERSION = %u. "
               "Using an unknown LZ_API_VERSION\n", LZ_API_VERSION );
#endif
  }


void show_version()
  {
  std::printf( "%s %s\n", program_name, PROGVERSION );
  std::printf( "Copyright (C) %s Antonio Diaz Diaz.\n", program_year );
  std::fputs( "License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl.html>\n"
              "This is free software: you are free to change and redistribute it.\n"
              "There is NO WARRANTY, to the extent permitted by law.\n", stdout );
  show_lzlib_version();
  }


int check_lzlib_ver()	// <major>.<minor> or <major>.<minor>[a-z.-]*
  {
#if defined LZ_API_VERSION && LZ_API_VERSION >= 1012
  const unsigned char * p = (unsigned char *)LZ_version_string;
  unsigned major = 0, minor = 0;
  while( major < 100000 && isdigit( *p ) )
    { major *= 10; major += *p - '0'; ++p; }
  if( *p == '.' ) ++p;
  else
out: { show_error( "Invalid LZ_version_string in lzlib.h" ); return 2; }
  while( minor < 100 && isdigit( *p ) )
    { minor *= 10; minor += *p - '0'; ++p; }
  if( *p && *p != '-' && *p != '.' && !std::islower( *p ) ) goto out;
  const unsigned version = major * 1000 + minor;
  if( LZ_API_VERSION != version )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: Version mismatch in lzlib.h: "
                    "LZ_API_VERSION = %u, should be %u.\n",
                    program_name, LZ_API_VERSION, version );
    return 2;
    }
#endif
  return 0;
  }


int check_lib()
  {
  int retval = check_lzlib_ver();
  if( std::strcmp( LZ_version_string, LZ_version() ) != 0 )
    { set_retval( retval, 1 );
      if( verbosity >= 0 )
        std::printf( "warning: LZ_version_string != LZ_version() (%s vs %s)\n",
                     LZ_version_string, LZ_version() ); }
#if defined LZ_API_VERSION && LZ_API_VERSION >= 1012
  if( LZ_API_VERSION != LZ_api_version() )
    { set_retval( retval, 1 );
      if( verbosity >= 0 )
        std::printf( "warning: LZ_API_VERSION != LZ_api_version() (%u vs %u)\n",
                     LZ_API_VERSION, LZ_api_version() ); }
#endif
  if( verbosity >= 1 ) show_lzlib_version();
  return retval;
  }


int chvalue( const unsigned char ch )
  {
  if( ch >= '0' && ch <= '9' ) return ch - '0';
  if( ch >= 'A' && ch <= 'Z' ) return ch - 'A' + 10;
  if( ch >= 'a' && ch <= 'z' ) return ch - 'a' + 10;
  return 255;
  }

long long strtoll_( const char * const ptr, const char ** tail, int base )
  {
  if( tail ) *tail = ptr;				// error value
  int i = 0;
  while( std::isspace( ptr[i] ) || (unsigned char)ptr[i] == 0xA0 ) ++i;
  const bool minus = ptr[i] == '-';
  if( minus || ptr[i] == '+' ) ++i;
  if( base < 0 || base > 36 || base == 1 ||
      ( base == 0 && !std::isdigit( ptr[i] ) ) ||
      ( base != 0 && chvalue( ptr[i] ) >= base ) )
    { errno = EINVAL; return 0; }

  if( base == 0 )
    {
    if( ptr[i] != '0' ) base = 10;			// decimal
    else if( ptr[i+1] == 'x' || ptr[i+1] == 'X' ) { base = 16; i += 2; }
    else base = 8;					// octal or 0
    }
  const int dpg = ( base != 16 ) ? 3 : 2;	// min digits per group
  int dig = dpg - 1;	// digits in current group, first may have 1 digit
  const unsigned long long limit = minus ? LLONG_MAX + 1ULL : LLONG_MAX;
  unsigned long long result = 0;
  bool erange = false;
  for( ; ptr[i]; ++i )
    {
    if( ptr[i] == '_' ) { if( dig < dpg ) break; else { dig = 0; continue; } }
    const int val = chvalue( ptr[i] ); if( val >= base ) break; else ++dig;
    if( !erange && ( limit - val ) / base >= result )
      result = result * base + val;
    else erange = true;
    }
  if( dig < dpg ) { errno = EINVAL; return 0; }
  if( tail ) *tail = ptr + i;
  if( erange ) { errno = ERANGE; return minus ? LLONG_MIN : LLONG_MAX; }
  return minus ? 0LL - result : result;
  }


// separate numbers of 5 or more digits in groups of 3 digits using '_'
const char * format_num3p( long long num )
  {
  enum { buffers = 8, bufsize = 4 * sizeof num };
  const char * const si_prefix = "kMGTPEZYRQ";
  const char * const binary_prefix = "KMGTPEZYRQ";
  static char buffer[buffers][bufsize];	// circle of buffers for printf
  static int current = 0;

  char * const buf = buffer[current++]; current %= buffers;
  char * p = buf + bufsize - 1;		// fill the buffer backwards
  *p = 0;				// terminator
  const bool negative = num < 0;
  if( num >= 10000 || num <= -10000 )
    {
    char prefix = 0;			// try binary first, then si
    for( int i = 0; num != 0 && num % 1024 == 0 && binary_prefix[i]; ++i )
      { num /= 1024; prefix = binary_prefix[i]; }
    if( prefix ) *(--p) = 'i';
    else
      for( int i = 0; num != 0 && num % 1000 == 0 && si_prefix[i]; ++i )
        { num /= 1000; prefix = si_prefix[i]; }
    if( prefix ) *(--p) = prefix;
    }
  const bool split = num >= 10000 || num <= -10000;

  for( int i = 0; ; )
    {
    const long long onum = num; num /= 10;
    *(--p) = llabs( onum - ( 10 * num ) ) + '0'; if( num == 0 ) break;
    if( split && ++i >= 3 ) { i = 0; *(--p) = '_'; }
    }
  if( negative ) *(--p) = '-';
  return p;
  }


void show_option_error( const char * const arg, const char * const msg,
                        const char * const option_name )
  {
  if( verbosity >= 0 )
    std::fprintf( stderr, "%s: '%s': %s option '%s'.\n",
                  program_name, arg, msg, option_name );
  }


// Recognized formats: <num>k, <num>Ki, <num>[MGTPEZYRQ][i]
long long getnum( const char * const arg, const char * const option_name,
                  const long long llimit = LLONG_MIN,
                  const long long ulimit = LLONG_MAX )
  {
  const char * tail;
  errno = 0;
  long long result = strtoll_( arg, &tail, 0 );
  if( tail == arg )
    { show_option_error( arg, "Bad or missing numerical argument in",
                         option_name ); std::exit( 1 ); }

  if( !errno && *tail )
    {
    const int factor = (tail[1] == 'i') ? 1024 : 1000;
    int exponent = 0;				// 0 = bad multiplier
    switch( *tail )
      {
      case 'Q': exponent = 10; break;
      case 'R': exponent = 9; break;
      case 'Y': exponent = 8; break;
      case 'Z': exponent = 7; break;
      case 'E': exponent = 6; break;
      case 'P': exponent = 5; break;
      case 'T': exponent = 4; break;
      case 'G': exponent = 3; break;
      case 'M': exponent = 2; break;
      case 'K': if( factor == 1024 ) exponent = 1; break;
      case 'k': if( factor == 1000 ) exponent = 1; break;
      }
    if( exponent <= 0 )
      { show_option_error( arg, "Bad multiplier in numerical argument of",
                           option_name ); std::exit( 1 ); }
    for( int i = 0; i < exponent; ++i )
      {
      if( ( result >= 0 && LLONG_MAX / factor >= result ) ||
          ( result < 0 && LLONG_MIN / factor <= result ) ) result *= factor;
      else { errno = ERANGE; break; }
      }
    }
  if( !errno && ( result < llimit || result > ulimit ) ) errno = ERANGE;
  if( errno )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: '%s': Value out of limits [%s,%s] in "
                    "option '%s'.\n", program_name, arg, format_num3p( llimit ),
                    format_num3p( ulimit ), option_name );
    std::exit( 1 );
    }
  return result;
  }


bool contains_control( const char * const filename )
  {
  for( const char * p = filename; *p; ++p )
    if( ( *p <= 13 && *p >= 7 ) || *p == 27 || *p == 127 ) return true;
  return false;
  }

void set_archive_name( std::string & archive_name, const std::string & new_name )
  {
  static bool first_call = true;

  if( !first_call )
    { show_error( "Only one archive can be specified.", 0, true );
      std::exit( 1 ); }
  first_call = false;
  if( new_name == "-" ) return;
  if( !contains_control( new_name.c_str() ) )
    { archive_name = new_name; return; }
  show_file_error( new_name.c_str(),
                   "Control characters not allowed in archive name." );
  std::exit( 1 );
  }


void set_mode( Program_mode & program_mode, const Program_mode new_mode )
  {
  if( program_mode != m_none && program_mode != new_mode )
    {
    show_error( "Only one operation can be specified.", 0, true );
    std::exit( 1 );
    }
  program_mode = new_mode;
  }


// parse time as 'long long' even if time_t is 32-bit
long long parse_mtime( const char * arg, const char * const pn )
  {
  if( *arg == '@' ) return getnum( arg + 1, pn );  // seconds since the epoch
  else if( *arg == '.' || *arg == '/' )
    {
    struct stat st;
    if( stat( arg, &st ) == 0 ) return st.st_mtime;
    show_file_error( arg, "Can't stat mtime reference file", errno );
    std::exit( 1 );
    }
  else		// format '[-]YYYY-MM-DD[[[<separator>HH]:MM]:SS]'
    {
    long long y;	// long long because 2147483648-01-01 overflows int
    unsigned mo, d, h, m, s;
    char sep;
    const int n = std::sscanf( arg, "%lld-%u-%u%c%u:%u:%u",
                               &y, &mo, &d, &sep, &h, &m, &s );
    if( n >= 3 && n <= 7 && n != 4 && ( n == 3 || sep == ' ' || sep == 'T' ) )
      {
      if( y >= INT_MIN + 1900 && y <= INT_MAX && mo >= 1 && mo <= 12 )
        {
        struct tm t;
        t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
        t.tm_hour = (n >= 5) ? h : 0; t.tm_min = (n >= 6) ? m : 0;
        t.tm_sec = (n >= 7) ? s : 0; t.tm_isdst = -1; t.tm_wday = -1;
        const long long mtime = std::mktime( &t );
        if( mtime != -1 || t.tm_wday != -1 ) return mtime;	// valid mtime
        }
      show_option_error( arg, "Date out of limits in", pn ); std::exit( 1 );
      }
    }
  show_option_error( arg, "Unknown date format in", pn ); std::exit( 1 );
  }


long long parse_owner( const char * const arg, const char * const pn )
  {
  const struct passwd * const pw = getpwnam( arg );
  if( pw ) return pw->pw_uid;
  if( std::isdigit( (unsigned char)arg[0] ) )
    return getnum( arg, pn, 0, LLONG_MAX );
  if( std::strcmp( arg, "root" ) == 0 ) return 0;
  show_option_error( arg, "Invalid owner in", pn ); std::exit( 1 );
  }

long long parse_group( const char * const arg, const char * const pn )
  {
  const struct group * const gr = getgrnam( arg );
  if( gr ) return gr->gr_gid;
  if( std::isdigit( (unsigned char)arg[0] ) )
    return getnum( arg, pn, 0, LLONG_MAX );
  if( std::strcmp( arg, "root" ) == 0 ) return 0;
  show_option_error( arg, "Invalid group in", pn ); std::exit( 1 );
  }

} // end namespace

// separate numbers of 5 or more digits in groups of 3 digits using '_'
const char * format_num3( unsigned long long num, const bool negative )
  {
  enum { buffers = 8, bufsize = 4 * sizeof num };
  static char buffer[buffers][bufsize];	// circle of buffers for printf
  static int current = 0;

  char * const buf = buffer[current++]; current %= buffers;
  char * p = buf + bufsize - 1;		// fill the buffer backwards
  *p = 0;				// terminator
  const bool split = num >= 10000;

  for( int i = 0; ; )
    {
    *(--p) = num % 10 + '0'; num /= 10; if( num == 0 ) break;
    if( split && ++i >= 3 ) { i = 0; *(--p) = '_'; }
    }
  if( negative ) *(--p) = '-';
  return p;
  }


int hstat( const char * const filename, struct stat * const st,
           const bool dereference )
  { return dereference ? stat( filename, st ) : lstat( filename, st ); }


bool nonempty_arg( const Arg_parser & parser, const int i )
  { return parser.code( i ) == 0 && !parser.argument( i ).empty(); }


int open_instream( const char * const name, struct stat * const in_statsp )
  {
  const int infd = open( name, O_RDONLY | O_BINARY );
  if( infd < 0 ) { show_file_error( name, rd_open_msg, errno ); return -1; }
  struct stat st;
  struct stat * const stp = in_statsp ? in_statsp : &st;
  if( fstat( infd, stp ) == 0 && S_ISDIR( stp->st_mode ) )
    { show_file_error( name, "Can't read. Is a directory." );
      close( infd ); return -1; }	// infd must not be a directory
  return infd;
  }


int open_outstream( const std::string & name, const bool create,
                    Resizable_buffer * const rbufp, const bool force )
  {
  const int cflags = O_CREAT | O_WRONLY | ( force ? O_TRUNC : O_EXCL );
  const int flags = ( create ? cflags : O_RDWR ) | O_BINARY;
  const mode_t outfd_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

  const int outfd = open( name.c_str(), flags, outfd_mode );
  if( outfd < 0 )
    {
    const char * msg = !create ? "Error opening file" :
      ( ( errno == EEXIST ) ? "Skipping file" : "Can't create file" );
    if( !rbufp ) show_file_error( name.c_str(), msg, errno );
    else format_file_error( *rbufp, name.c_str(), msg, errno );
    }
  return outfd;
  }


void show_error( const char * const msg, const int errcode, const bool help )
  {
  if( verbosity < 0 ) return;
  if( msg && msg[0] )
    std::fprintf( stderr, "%s: %s%s%s\n", program_name, msg,
                  ( errcode > 0 ) ? ": " : "",
                  ( errcode > 0 ) ? std::strerror( errcode ) : "" );
  if( help )
    std::fprintf( stderr, "Try '%s --help' for more information.\n",
                  invocation_name );
  }


bool format_error( Resizable_buffer & rbuf, const int errcode,
                   const char * const format, ... )
  {
  if( verbosity < 0 ) { rbuf.resize( 1 ); rbuf()[0] = 0; return false; }
  va_list args;
  for( int i = 0; i < 2; ++i )		// resize rbuf if not large enough
    {
    int len = snprintf( rbuf(), rbuf.size(), "%s: ", program_name );
    if( !rbuf.resize( len + 1 ) ) break;
    va_start( args, format );
    len += vsnprintf( rbuf() + len, rbuf.size() - len, format, args );
    va_end( args );
    if( !rbuf.resize( len + 2 ) ) break;
    if( errcode <= 0 ) { rbuf()[len++] = '\n'; rbuf()[len] = 0; }
    else len += snprintf( rbuf() + len, rbuf.size() - len, ": %s\n",
                          std::strerror( errcode ) );
    if( len < (int)rbuf.size() ) return true;
    if( i > 0 || !rbuf.resize( len + 1 ) ) break;
    }
  return false;
  }


bool format_error( std::string & msg, const int errcode,
                   const char * const format, ... )
  {
  if( verbosity < 0 ) return false;
  Resizable_buffer rbuf;
  if( !rbuf.size() ) return false;
  va_list args;
  for( int i = 0; i < 2; ++i )		// resize rbuf if not large enough
    {
    int len = snprintf( rbuf(), rbuf.size(), "%s: ", program_name );
    if( !rbuf.resize( len + 1 ) ) break;
    va_start( args, format );
    len += vsnprintf( rbuf() + len, rbuf.size() - len, format, args );
    va_end( args );
    if( !rbuf.resize( len + 2 ) ) break;
    if( errcode <= 0 ) { rbuf()[len++] = '\n'; rbuf()[len] = 0; }
    else len += snprintf( rbuf() + len, rbuf.size() - len, ": %s\n",
                          std::strerror( errcode ) );
    if( len < (int)rbuf.size() ) { msg.assign( rbuf(), len ); return true; }
    if( i > 0 || !rbuf.resize( len + 1 ) ) break;
    }
  return false;
  }


void print_error( const int errcode, const char * const format, ... )
  {
  if( verbosity < 0 ) return;
  va_list args;
  std::fprintf( stderr, "%s: ", program_name );
  va_start( args, format );
  std::vfprintf( stderr, format, args );
  va_end( args );
  if( errcode <= 0 ) std::fputc( '\n', stderr );
  else std::fprintf( stderr, ": %s\n", std::strerror( errcode ) );
  }


void format_file_error( std::string & estr, const char * const filename,
                        const char * const msg, const int errcode )
  {
  if( verbosity < 0 ) return;
  estr += program_name; estr += ": "; estr += filename; estr += ": ";
  estr += msg;
  if( errcode > 0 ) { estr += ": "; estr += std::strerror( errcode ); }
  estr += '\n';
  }

bool format_file_error( Resizable_buffer & rbuf, const char * const filename,
                        const char * const msg, const int errcode )
  {
  if( verbosity < 0 ) { rbuf.resize( 1 ); rbuf()[0] = 0; return false; }
  for( int i = 0; i < 2; ++i )		// resize rbuf if not large enough
    {
    const int len = snprintf( rbuf(), rbuf.size(), "%s: %s: %s%s%s\n",
          program_name, filename, msg, ( errcode > 0 ) ? ": " : "",
          ( errcode > 0 ) ? std::strerror( errcode ) : "" );
    if( len < (int)rbuf.size() || !rbuf.resize( len + 1 ) ) break;
    }
  return true;
  }

void show_file_error( const char * const filename, const char * const msg,
                      const int errcode )
  {
  if( verbosity >= 0 && msg && msg[0] )
    std::fprintf( stderr, "%s: %s: %s%s%s\n", program_name,
                  filename, msg, ( errcode > 0 ) ? ": " : "",
                  ( errcode > 0 ) ? std::strerror( errcode ) : "" );
  }


void internal_error( const char * const msg )
  {
  if( verbosity >= 0 )
    std::fprintf( stderr, "%s: internal error: %s\n", program_name, msg );
  std::exit( 3 );
  }


int main( const int argc, const char * const argv[] )
  {
  if( argc > 0 ) invocation_name = argv[0];

  enum { opt_ano = 256, opt_aso, opt_bso, opt_chk, opt_crc, opt_dbg, opt_del,
         opt_dep, opt_dso, opt_exc, opt_grp, opt_iid, opt_imd, opt_kd,
         opt_mnt, opt_mti, opt_nso, opt_num, opt_ofl, opt_out, opt_own,
         opt_par, opt_per, opt_rec, opt_sol, opt_tb, opt_un, opt_wn, opt_xdv };
  const Arg_parser::Option options[] =
    {
    { '0', 0,                      Arg_parser::no  },
    { '1', 0,                      Arg_parser::no  },
    { '2', 0,                      Arg_parser::no  },
    { '3', 0,                      Arg_parser::no  },
    { '4', 0,                      Arg_parser::no  },
    { '5', 0,                      Arg_parser::no  },
    { '6', 0,                      Arg_parser::no  },
    { '7', 0,                      Arg_parser::no  },
    { '8', 0,                      Arg_parser::no  },
    { '9', 0,                      Arg_parser::no  },
    { '?', "help",                 Arg_parser::no  },
    { 'A', "concatenate",          Arg_parser::no  },
    { 'B', "data-size",            Arg_parser::yes },
    { 'c', "create",               Arg_parser::no  },
    { 'C', "directory",            Arg_parser::yes },
    { 'd', "diff",                 Arg_parser::no  },
    { 'f', "file",                 Arg_parser::yes },
    { 'h', "dereference",          Arg_parser::no  },
    { 'H', "format",               Arg_parser::yes },
    { 'n', "threads",              Arg_parser::yes },
    { 'o', "output",               Arg_parser::yes },
    { 'p', "preserve-permissions", Arg_parser::no  },
    { 'q', "quiet",                Arg_parser::no  },
    { 'r', "append",               Arg_parser::no  },
    { 'R', "no-recursive",         Arg_parser::no  },
    { 't', "list",                 Arg_parser::no  },
    { 'T', "files-from",           Arg_parser::yes },
    { 'v', "verbose",              Arg_parser::no  },
    { 'V', "version",              Arg_parser::no  },
    { 'x', "extract",              Arg_parser::no  },
    { 'z', "compress",             Arg_parser::no  },
    { opt_ano, "anonymous",        Arg_parser::no  },
    { opt_aso, "asolid",           Arg_parser::no  },
    { opt_bso, "bsolid",           Arg_parser::no  },
    { opt_chk, "check-lib",        Arg_parser::no  },
    { opt_dbg, "debug",            Arg_parser::yes },
    { opt_del, "delete",           Arg_parser::no  },
    { opt_dep, "depth",            Arg_parser::no  },
    { opt_dso, "dsolid",           Arg_parser::no  },
    { opt_exc, "exclude",          Arg_parser::yes },
    { opt_grp, "group",            Arg_parser::yes },
    { opt_iid, "ignore-ids",       Arg_parser::no  },
    { opt_imd, "ignore-metadata",  Arg_parser::no  },
    { opt_kd,  "keep-damaged",     Arg_parser::no  },
    { opt_crc, "missing-crc",      Arg_parser::no  },
    { opt_mnt, "mount",            Arg_parser::no  },
    { opt_mti, "mtime",            Arg_parser::yes },
    { opt_nso, "no-solid",         Arg_parser::no  },
    { opt_num, "numeric-owner",    Arg_parser::no  },
    { opt_ofl, "ignore-overflow",  Arg_parser::no  },
    { opt_out, "out-slots",        Arg_parser::yes },
    { opt_own, "owner",            Arg_parser::yes },
    { opt_par, "parallel",         Arg_parser::no  },
    { opt_per, "permissive",       Arg_parser::no  },
    { opt_rec, "recursive",        Arg_parser::no  },
    { opt_sol, "solid",            Arg_parser::no  },
    { opt_tb, "time-bits",         Arg_parser::no  },
    { opt_un, "uncompressed",      Arg_parser::no  },
    { opt_wn, "warn-newer",        Arg_parser::no  },
    { opt_xdv, "xdev",             Arg_parser::no  },
    { 0, 0,                        Arg_parser::no  } };

  const Arg_parser parser( argc, argv, options, Arg_parser::in_order );
  if( parser.error().size() )				// bad option
    { show_error( parser.error().c_str(), 0, true ); return 1; }
  Cl_options cl_opts( parser );

  const long num_online = std::max( 1L, sysconf( _SC_NPROCESSORS_ONLN ) );
  long max_workers = sysconf( _SC_THREAD_THREADS_MAX );
  if( max_workers < 1 || max_workers > INT_MAX / (int)sizeof (pthread_t) )
    max_workers = INT_MAX / sizeof (pthread_t);

  const char * f_pn = 0;
  const char * o_pn = 0;
  const char * z_pn = 0;
  for( int argind = 0; argind < parser.arguments(); ++argind )
    {
    const int code = parser.code( argind );
    const std::string & sarg = parser.argument( argind );
    if( !code )					// check and skip non-options
      {
      if( sarg.empty() )
        { show_error( "Empty non-option argument." ); return 1; }
      if( sarg != "-" && ++cl_opts.num_files == 0 )
        { show_error( "Too many files." ); return 1; }
      continue;
      }
    const char * const arg = sarg.c_str();
    const char * const pn = parser.parsed_name( argind ).c_str();
    switch( code )
      {
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
                cl_opts.set_level( code - '0' ); break;
      case '?': show_help( num_online ); return 0;
      case 'A': set_mode( cl_opts.program_mode, m_concatenate ); break;
      case 'B': cl_opts.data_size =
                  getnum( arg, pn, min_data_size, max_data_size ); break;
      case 'c': set_mode( cl_opts.program_mode, m_create ); break;
      case 'C': cl_opts.option_C_present = true; break;
      case 'd': set_mode( cl_opts.program_mode, m_diff ); break;
      case 'f': set_archive_name( cl_opts.archive_name, sarg ); f_pn = pn; break;
      case 'h': cl_opts.dereference = true; break;
      case 'H': break;					// ignore format
      case 'n': cl_opts.num_workers = getnum( arg, pn, 0, max_workers ); break;
      case 'o': cl_opts.output_filename = sarg; o_pn = pn; break;
      case 'p': cl_opts.preserve_permissions = true; break;
      case 'q': verbosity = -1; break;
      case 'r': set_mode( cl_opts.program_mode, m_append ); break;
      case 'R': cl_opts.recursive = false; break;
      case 't': set_mode( cl_opts.program_mode, m_list ); break;
      case 'T': cl_opts.option_T_present = true; break;
      case 'v': if( verbosity < 4 ) ++verbosity; break;
      case 'V': show_version(); return 0;
      case 'x': set_mode( cl_opts.program_mode, m_extract ); break;
      case 'z': set_mode( cl_opts.program_mode, m_compress ); z_pn = pn; break;
      case opt_ano: cl_opts.uid = parse_owner( "root", pn );
                    cl_opts.gid = parse_group( "root", pn ); break;
      case opt_aso: cl_opts.solidity = asolid; break;
      case opt_bso: cl_opts.solidity = bsolid; break;
      case opt_crc: cl_opts.missing_crc = true; break;
      case opt_chk: return check_lib();
      case opt_dbg: cl_opts.debug_level = getnum( arg, pn, 0, 3 ); break;
      case opt_del: set_mode( cl_opts.program_mode, m_delete ); break;
      case opt_dep: cl_opts.depth = true; break;
      case opt_dso: cl_opts.solidity = dsolid; break;
      case opt_exc: Exclude::add_pattern( sarg ); break;
      case opt_grp: cl_opts.gid = parse_group( arg, pn ); break;
      case opt_iid: cl_opts.ignore_ids = true; break;
      case opt_imd: cl_opts.ignore_metadata = true; break;
      case opt_kd:  cl_opts.keep_damaged = true; break;
      case opt_mnt: cl_opts.mount = true; break;
      case opt_mti: cl_opts.mtime = parse_mtime( arg, pn );
                    cl_opts.mtime_set = true; break;
      case opt_nso: cl_opts.solidity = no_solid; break;
      case opt_num: cl_opts.numeric_owner = true; break;
      case opt_ofl: cl_opts.ignore_overflow = true; break;
      case opt_out: cl_opts.out_slots = getnum( arg, pn, 1, 1024 ); break;
      case opt_own: cl_opts.uid = parse_owner( arg, pn ); break;
      case opt_par: cl_opts.parallel = true; break;
      case opt_per: cl_opts.permissive = true; break;
      case opt_rec: cl_opts.recursive = true; break;
      case opt_sol: cl_opts.solidity = solid; break;
      case opt_tb: std::printf( "%u\n", (int)sizeof( time_t ) * 8 ); return 0;
      case opt_un: cl_opts.set_level( -1 ); break;
      case opt_wn: cl_opts.warn_newer = true; break;
      case opt_xdv: cl_opts.xdev = true; break;
      default: internal_error( "uncaught option." );
      }
    } // end process options

  if( cl_opts.program_mode != m_compress && o_pn )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: Option '%s' can only be used with "
                    "'-z, --compress'.\n", program_name, o_pn );
    return 1;
    }
  if( cl_opts.program_mode == m_compress && f_pn )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: Option '%s' can't be used with '%s'.\n",
                    program_name, f_pn, z_pn );
    return 1;
    }
  if( cl_opts.program_mode == m_compress && cl_opts.uncompressed() )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: Option '--uncompressed' can't be used with '%s'.\n",
                    program_name, z_pn );
    return 1;
    }

#if !defined LZ_API_VERSION || LZ_API_VERSION < 1012	// compile-time test
#error "lzlib 1.12 or newer needed."
#endif
  if( LZ_api_version() < 1012 )				// runtime test
    { show_error( "Wrong library version. At least lzlib 1.12 is required." );
      return 1; }

#if defined __OS2__
  setmode( STDIN_FILENO, O_BINARY );
  setmode( STDOUT_FILENO, O_BINARY );
#endif

  if( cl_opts.data_size <= 0 && !cl_opts.uncompressed() )
    {
    if( cl_opts.level == 0 ) cl_opts.data_size = 1 << 20;
    else cl_opts.data_size = 2 * option_mapping[cl_opts.level].dictionary_size;
    }
  if( cl_opts.num_workers < 0 )			// 0 disables multithreading
    cl_opts.num_workers = std::min( num_online, max_workers );

  switch( cl_opts.program_mode )
    {
    case m_none:        show_error( "Missing operation.", 0, true ); return 1;
    case m_append:
    case m_create:      return encode( cl_opts );
    case m_compress:    return compress( cl_opts );
    case m_concatenate: return concatenate( cl_opts );
    case m_delete:      tzset(); return delete_members( cl_opts );
    case m_diff:
    case m_extract:
    case m_list:        tzset(); return decode( cl_opts );
    }
  }
