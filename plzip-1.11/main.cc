/* Plzip - Massively parallel implementation of lzip
   Copyright (C) 2009 Laszlo Ersek.
   Copyright (C) 2009-2024 Antonio Diaz Diaz.

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
   (file not found, invalid command-line options, I/O errors, etc), 2 to
   indicate a corrupt or invalid input file, 3 for an internal consistency
   error (e.g., bug) which caused plzip to panic.
*/

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <cerrno>
#include <climits>		// SSIZE_MAX
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <stdint.h>		// SIZE_MAX
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <lzlib.h>
#if defined __MSVCRT__ || defined __OS2__
#include <io.h>
#if defined __MSVCRT__
#define fchmod(x,y) 0
#define fchown(x,y,z) 0
#define strtoull std::strtoul
#define SIGHUP SIGTERM
#define S_ISSOCK(x) 0
#ifndef S_IRGRP
#define S_IRGRP 0
#define S_IWGRP 0
#define S_IROTH 0
#define S_IWOTH 0
#endif
#endif
#endif

#include "arg_parser.h"
#include "lzip.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#if CHAR_BIT != 8
#error "Environments where CHAR_BIT != 8 are not supported."
#endif

#if ( defined  SIZE_MAX &&  SIZE_MAX < UINT_MAX ) || \
    ( defined SSIZE_MAX && SSIZE_MAX <  INT_MAX )
#error "Environments where 'size_t' is narrower than 'int' are not supported."
#endif

int verbosity = 0;

namespace {

const char * const program_name = "plzip";
const char * const program_year = "2024";
const char * invocation_name = program_name;		// default value

const struct { const char * from; const char * to; } known_extensions[] = {
  { ".lz",  ""     },
  { ".tlz", ".tar" },
  { 0,      0      } };

struct Lzma_options
  {
  int dictionary_size;		// 4 KiB .. 512 MiB
  int match_len_limit;		// 5 .. 273
  };

enum Mode { m_compress, m_decompress, m_list, m_test };

/* Variables used in signal handler context.
   They are not declared volatile because the handler never returns. */
std::string output_filename;
int outfd = -1;
bool delete_output_on_interrupt = false;


void show_help( const long num_online )
  {
  std::printf( "Plzip is a massively parallel (multi-threaded) implementation of lzip,\n"
               "compatible with lzip 1.4 or newer. Plzip uses the compression library lzlib.\n"
               "\nLzip is a lossless data compressor with a user interface similar to the one\n"
               "of gzip or bzip2. Lzip uses a simplified form of the 'Lempel-Ziv-Markov\n"
               "chain-Algorithm' (LZMA) stream format to maximize interoperability. The\n"
               "maximum dictionary size is 512 MiB so that any lzip file can be decompressed\n"
               "on 32-bit machines. Lzip provides accurate and robust 3-factor integrity\n"
               "checking. Lzip can compress about as fast as gzip (lzip -0) or compress most\n"
               "files more than bzip2 (lzip -9). Decompression speed is intermediate between\n"
               "gzip and bzip2. Lzip is better than gzip and bzip2 from a data recovery\n"
               "perspective. Lzip has been designed, written, and tested with great care to\n"
               "replace gzip and bzip2 as the standard general-purpose compressed format for\n"
               "Unix-like systems.\n"
               "\nPlzip can compress/decompress large files on multiprocessor machines much\n"
               "faster than lzip, at the cost of a slightly reduced compression ratio (0.4\n"
               "to 2 percent larger compressed files). Note that the number of usable\n"
               "threads is limited by file size; on files larger than a few GB plzip can use\n"
               "hundreds of processors, but on files of only a few MB plzip is no faster\n"
               "than lzip.\n"
               "\nUsage: %s [options] [files]\n", invocation_name );
  std::printf( "\nOptions:\n"
               "  -h, --help                     display this help and exit\n"
               "  -V, --version                  output version information and exit\n"
               "  -a, --trailing-error           exit with error status if trailing data\n"
               "  -B, --data-size=<bytes>        set size of input data blocks [2x8=16 MiB]\n"
               "  -c, --stdout                   write to standard output, keep input files\n"
               "  -d, --decompress               decompress, test compressed file integrity\n"
               "  -f, --force                    overwrite existing output files\n"
               "  -F, --recompress               force re-compression of compressed files\n"
               "  -k, --keep                     keep (don't delete) input files\n"
               "  -l, --list                     print (un)compressed file sizes\n"
               "  -m, --match-length=<bytes>     set match length limit in bytes [36]\n"
               "  -n, --threads=<n>              set number of (de)compression threads [%ld]\n"
               "  -o, --output=<file>            write to <file>, keep input files\n"
               "  -q, --quiet                    suppress all messages\n"
               "  -s, --dictionary-size=<bytes>  set dictionary size limit in bytes [8 MiB]\n"
               "  -t, --test                     test compressed file integrity\n"
               "  -v, --verbose                  be verbose (a 2nd -v gives more)\n"
               "  -0 .. -9                       set compression level [default 6]\n"
               "      --fast                     alias for -0\n"
               "      --best                     alias for -9\n"
               "      --loose-trailing           allow trailing data seeming corrupt header\n"
               "      --in-slots=<n>             number of 1 MiB input packets buffered [4]\n"
               "      --out-slots=<n>            number of 1 MiB output packets buffered [64]\n"
               "      --check-lib                compare version of lzlib.h with liblz.{a,so}\n",
               num_online );
  if( verbosity >= 1 )
    {
    std::printf( "      --debug=<level>        print mode(2), debug statistics(1) to stderr\n" );
    }
  std::printf( "\nIf no file names are given, or if a file is '-', plzip compresses or\n"
               "decompresses from standard input to standard output.\n"
               "Numbers may be followed by a multiplier: k = kB = 10^3 = 1000,\n"
               "Ki = KiB = 2^10 = 1024, M = 10^6, Mi = 2^20, G = 10^9, Gi = 2^30, etc...\n"
               "Dictionary sizes 12 to 29 are interpreted as powers of two, meaning 2^12 to\n"
               "2^29 bytes.\n"
               "\nThe bidimensional parameter space of LZMA can't be mapped to a linear scale\n"
               "optimal for all files. If your files are large, very repetitive, etc, you\n"
               "may need to use the options --dictionary-size and --match-length directly\n"
               "to achieve optimal performance.\n"
               "\nTo extract all the files from archive 'foo.tar.lz', use the commands\n"
               "'tar -xf foo.tar.lz' or 'plzip -cd foo.tar.lz | tar -xf -'.\n"
               "\nExit status: 0 for a normal exit, 1 for environmental problems\n"
               "(file not found, invalid command-line options, I/O errors, etc), 2 to\n"
               "indicate a corrupt or invalid input file, 3 for an internal consistency\n"
               "error (e.g., bug) which caused plzip to panic.\n"
               "\nReport bugs to lzip-bug@nongnu.org\n"
               "Plzip home page: http://www.nongnu.org/lzip/plzip.html\n" );
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
  std::printf( "Copyright (C) 2009 Laszlo Ersek.\n" );
  std::printf( "Copyright (C) %s Antonio Diaz Diaz.\n", program_year );
  std::printf( "License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl.html>\n"
               "This is free software: you are free to change and redistribute it.\n"
               "There is NO WARRANTY, to the extent permitted by law.\n" );
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

} // end namespace

void Pretty_print::operator()( const char * const msg ) const
  {
  if( verbosity < 0 ) return;
  if( first_post )
    {
    first_post = false;
    std::fputs( padded_name.c_str(), stderr );
    if( !msg ) std::fflush( stderr );
    }
  if( msg ) std::fprintf( stderr, "%s\n", msg );
  }


const char * bad_version( const unsigned version )
  {
  static char buf[80];
  snprintf( buf, sizeof buf, "Version %u member format not supported.",
            version );
  return buf;
  }


const char * format_ds( const unsigned dictionary_size )
  {
  enum { bufsize = 16, factor = 1024, n = 3 };
  static char buf[bufsize];
  const char * const prefix[n] = { "Ki", "Mi", "Gi" };
  const char * p = "";
  const char * np = "  ";
  unsigned num = dictionary_size;
  bool exact = ( num % factor == 0 );

  for( int i = 0; i < n && ( num > 9999 || ( exact && num >= factor ) ); ++i )
    { num /= factor; if( num % factor != 0 ) exact = false;
      p = prefix[i]; np = ""; }
  snprintf( buf, bufsize, "%s%4u %sB", np, num, p );
  return buf;
  }


void show_header( const unsigned dictionary_size )
  {
  std::fprintf( stderr, "dict %s, ", format_ds( dictionary_size ) );
  }

namespace {

// separate numbers of 5 or more digits in groups of 3 digits using '_'
const char * format_num3( unsigned long long num )
  {
  enum { buffers = 8, bufsize = 4 * sizeof num, n = 10 };
  const char * const si_prefix = "kMGTPEZYRQ";
  const char * const binary_prefix = "KMGTPEZYRQ";
  static char buffer[buffers][bufsize];	// circle of static buffers for printf
  static int current = 0;

  char * const buf = buffer[current++]; current %= buffers;
  char * p = buf + bufsize - 1;		// fill the buffer backwards
  *p = 0;	// terminator
  if( num > 1024 )
    {
    char prefix = 0;			// try binary first, then si
    for( int i = 0; i < n && num != 0 && num % 1024 == 0; ++i )
      { num /= 1024; prefix = binary_prefix[i]; }
    if( prefix ) *(--p) = 'i';
    else
      for( int i = 0; i < n && num != 0 && num % 1000 == 0; ++i )
        { num /= 1000; prefix = si_prefix[i]; }
    if( prefix ) *(--p) = prefix;
    }
  const bool split = num >= 10000;

  for( int i = 0; ; )
    {
    *(--p) = num % 10 + '0'; num /= 10; if( num == 0 ) break;
    if( split && ++i >= 3 ) { i = 0; *(--p) = '_'; }
    }
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
unsigned long long getnum( const char * const arg,
                           const char * const option_name,
                           const unsigned long long llimit,
                           const unsigned long long ulimit )
  {
  char * tail;
  errno = 0;
  unsigned long long result = strtoull( arg, &tail, 0 );
  if( tail == arg )
    { show_option_error( arg, "Bad or missing numerical argument in",
                         option_name ); std::exit( 1 ); }

  if( !errno && tail[0] )
    {
    const unsigned factor = ( tail[1] == 'i' ) ? 1024 : 1000;
    int exponent = 0;				// 0 = bad multiplier
    switch( tail[0] )
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
      if( ulimit / factor >= result ) result *= factor;
      else { errno = ERANGE; break; }
      }
    }
  if( !errno && ( result < llimit || result > ulimit ) ) errno = ERANGE;
  if( errno )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: '%s': Value out of limits [%s,%s] in "
                    "option '%s'.\n", program_name, arg, format_num3( llimit ),
                    format_num3( ulimit ), option_name );
    std::exit( 1 );
    }
  return result;
  }


int get_dict_size( const char * const arg, const char * const option_name )
  {
  char * tail;
  const long bits = std::strtol( arg, &tail, 0 );
  if( bits >= LZ_min_dictionary_bits() &&
      bits <= LZ_max_dictionary_bits() && *tail == 0 )
    return 1 << bits;
  int dictionary_size = getnum( arg, option_name, LZ_min_dictionary_size(),
                                                  LZ_max_dictionary_size() );
  if( dictionary_size == 65535 ) ++dictionary_size;	// no fast encoder
  return dictionary_size;
  }


void set_mode( Mode & program_mode, const Mode new_mode )
  {
  if( program_mode != m_compress && program_mode != new_mode )
    {
    show_error( "Only one operation can be specified.", 0, true );
    std::exit( 1 );
    }
  program_mode = new_mode;
  }


int extension_index( const std::string & name )
  {
  for( int eindex = 0; known_extensions[eindex].from; ++eindex )
    {
    const std::string ext( known_extensions[eindex].from );
    if( name.size() > ext.size() &&
        name.compare( name.size() - ext.size(), ext.size(), ext ) == 0 )
      return eindex;
    }
  return -1;
  }


void set_c_outname( const std::string & name, const bool filenames_given,
                    const bool force_ext )
  {
  /* zupdate < 1.9 depends on lzip adding the extension '.lz' to name when
     reading from standard input. */
  output_filename = name;
  if( force_ext ||
      ( !filenames_given && extension_index( output_filename ) < 0 ) )
    output_filename += known_extensions[0].from;
  }


void set_d_outname( const std::string & name, const int eindex )
  {
  if( eindex >= 0 )
    {
    const std::string from( known_extensions[eindex].from );
    if( name.size() > from.size() )
      {
      output_filename.assign( name, 0, name.size() - from.size() );
      output_filename += known_extensions[eindex].to;
      return;
      }
    }
  output_filename = name; output_filename += ".out";
  if( verbosity >= 1 )
    std::fprintf( stderr, "%s: %s: Can't guess original name -- using '%s'\n",
                  program_name, name.c_str(), output_filename.c_str() );
  }

} // end namespace

int open_instream( const char * const name, struct stat * const in_statsp,
                   const bool one_to_one, const bool reg_only )
  {
  int infd = open( name, O_RDONLY | O_BINARY );
  if( infd < 0 )
    show_file_error( name, "Can't open input file", errno );
  else
    {
    const int i = fstat( infd, in_statsp );
    const mode_t mode = in_statsp->st_mode;
    const bool can_read = ( i == 0 && !reg_only &&
                            ( S_ISBLK( mode ) || S_ISCHR( mode ) ||
                              S_ISFIFO( mode ) || S_ISSOCK( mode ) ) );
    if( i != 0 || ( !S_ISREG( mode ) && ( !can_read || one_to_one ) ) )
      {
      if( verbosity >= 0 )
        std::fprintf( stderr, "%s: %s: Input file is not a regular file%s.\n",
                      program_name, name, ( can_read && one_to_one ) ?
                      ",\n  and neither '-c' nor '-o' were specified" : "" );
      close( infd );
      infd = -1;
      }
    }
  return infd;
  }

namespace {

int open_instream2( const char * const name, struct stat * const in_statsp,
                    const Mode program_mode, const int eindex,
                    const bool one_to_one, const bool recompress )
  {
  if( program_mode == m_compress && !recompress && eindex >= 0 )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: %s: Input file already has '%s' suffix.\n",
                    program_name, name, known_extensions[eindex].from );
    return -1;
    }
  return open_instream( name, in_statsp, one_to_one, false );
  }


bool make_dirs( const std::string & name )
  {
  int i = name.size();
  while( i > 0 && name[i-1] != '/' ) --i;	// remove last component
  while( i > 0 && name[i-1] == '/' ) --i;	// remove slash(es)
  const int dirsize = i;	// size of dirname without trailing slash(es)

  for( i = 0; i < dirsize; )	// if dirsize == 0, dirname is '/' or empty
    {
    while( i < dirsize && name[i] == '/' ) ++i;
    const int first = i;
    while( i < dirsize && name[i] != '/' ) ++i;
    if( first < i )
      {
      const std::string partial( name, 0, i );
      const mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      struct stat st;
      if( stat( partial.c_str(), &st ) == 0 )
        { if( !S_ISDIR( st.st_mode ) ) { errno = ENOTDIR; return false; } }
      else if( mkdir( partial.c_str(), mode ) != 0 && errno != EEXIST )
        return false;		// if EEXIST, another process created the dir
      }
    }
  return true;
  }


bool open_outstream( const bool force, const bool protect )
  {
  const mode_t usr_rw = S_IRUSR | S_IWUSR;
  const mode_t all_rw = usr_rw | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
  const mode_t outfd_mode = protect ? usr_rw : all_rw;
  int flags = O_CREAT | O_WRONLY | O_BINARY;
  if( force ) flags |= O_TRUNC; else flags |= O_EXCL;

  outfd = -1;
  if( output_filename.size() &&
      output_filename[output_filename.size()-1] == '/' ) errno = EISDIR;
  else {
    if( !protect && !make_dirs( output_filename ) )
      { show_file_error( output_filename.c_str(),
          "Error creating intermediate directory", errno ); return false; }
    outfd = open( output_filename.c_str(), flags, outfd_mode );
    if( outfd >= 0 ) { delete_output_on_interrupt = true; return true; }
    if( errno == EEXIST )
      { show_file_error( output_filename.c_str(),
          "Output file already exists, skipping." ); return false; }
    }
  show_file_error( output_filename.c_str(), "Can't create output file", errno );
  return false;
  }


void set_signals( void (*action)(int) )
  {
  std::signal( SIGHUP, action );
  std::signal( SIGINT, action );
  std::signal( SIGTERM, action );
  }

} // end namespace

/* This can be called from any thread, main thread or sub-threads alike,
   since they all call common helper functions like 'xlock' that call
   cleanup_and_fail() in case of an error.
*/
void cleanup_and_fail( const int retval )
  {
  // only one thread can delete and exit
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  set_signals( SIG_IGN );			// ignore signals
  pthread_mutex_lock( &mutex );		// ignore errors to avoid loop
  const int saved_verbosity = verbosity;
  verbosity = -1;		// suppress messages from other threads
  if( delete_output_on_interrupt )
    {
    delete_output_on_interrupt = false;
    if( saved_verbosity >= 0 )
      std::fprintf( stderr, "%s: %s: Deleting output file, if it exists.\n",
                    program_name, output_filename.c_str() );
    if( outfd >= 0 ) { close( outfd ); outfd = -1; }
    if( std::remove( output_filename.c_str() ) != 0 && errno != ENOENT &&
        saved_verbosity >= 0 )
      std::fprintf( stderr, "%s: warning: deletion of output file failed: %s\n",
                    program_name, std::strerror( errno ) );
    }
  std::exit( retval );
  }

namespace {

extern "C" void signal_handler( int )
  {
  show_error( "Control-C or similar caught, quitting." );
  cleanup_and_fail( 1 );
  }


bool check_tty_in( const char * const input_filename, const int infd,
                   const Mode program_mode, int & retval )
  {
  if( ( program_mode == m_decompress || program_mode == m_test ) &&
      isatty( infd ) )				// for example /dev/tty
    { show_file_error( input_filename,
                       "I won't read compressed data from a terminal." );
      close( infd ); set_retval( retval, 2 );
      if( program_mode != m_test ) cleanup_and_fail( retval );
      return false; }
  return true;
  }

bool check_tty_out( const Mode program_mode )
  {
  if( program_mode == m_compress && isatty( outfd ) )
    { show_file_error( output_filename.size() ?
                       output_filename.c_str() : "(stdout)",
                       "I won't write compressed data to a terminal." );
      return false; }
  return true;
  }


// Set permissions, owner, and times.
void close_and_set_permissions( const struct stat * const in_statsp )
  {
  bool warning = false;
  if( in_statsp )
    {
    const mode_t mode = in_statsp->st_mode;
    // fchown in many cases returns with EPERM, which can be safely ignored.
    if( fchown( outfd, in_statsp->st_uid, in_statsp->st_gid ) == 0 )
      { if( fchmod( outfd, mode ) != 0 ) warning = true; }
    else
      if( errno != EPERM ||
          fchmod( outfd, mode & ~( S_ISUID | S_ISGID | S_ISVTX ) ) != 0 )
        warning = true;
    }
  if( close( outfd ) != 0 )
    { show_file_error( output_filename.c_str(), "Error closing output file",
                       errno ); cleanup_and_fail( 1 ); }
  outfd = -1;
  delete_output_on_interrupt = false;
  if( in_statsp )
    {
    struct utimbuf t;
    t.actime = in_statsp->st_atime;
    t.modtime = in_statsp->st_mtime;
    if( utime( output_filename.c_str(), &t ) != 0 ) warning = true;
    }
  if( warning && verbosity >= 1 )
    show_file_error( output_filename.c_str(),
                     "warning: can't change output file attributes", errno );
  }

} // end namespace


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


void show_file_error( const char * const filename, const char * const msg,
                      const int errcode )
  {
  if( verbosity >= 0 )
    std::fprintf( stderr, "%s: %s: %s%s%s\n", program_name, filename, msg,
                  ( errcode > 0 ) ? ": " : "",
                  ( errcode > 0 ) ? std::strerror( errcode ) : "" );
  }


void internal_error( const char * const msg )
  {
  if( verbosity >= 0 )
    std::fprintf( stderr, "%s: internal error: %s\n", program_name, msg );
  std::exit( 3 );
  }


void show_progress( const unsigned long long packet_size,
                    const unsigned long long cfile_size,
                    const Pretty_print * const p )
  {
  static unsigned long long csize = 0;		// file_size / 100
  static unsigned long long pos = 0;
  static const Pretty_print * pp = 0;
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  static bool enabled = true;

  if( !enabled ) return;
  if( p )					// initialize static vars
    {
    if( verbosity < 2 || !isatty( STDERR_FILENO ) ) { enabled = false; return; }
    csize = cfile_size; pos = 0; pp = p;
    }
  if( pp )
    {
    xlock( &mutex );
    pos += packet_size;
    if( csize > 0 )
      std::fprintf( stderr, "%4llu%%  %.1f MB\r", pos / csize, pos / 1000000.0 );
    else
      std::fprintf( stderr, "  %.1f MB\r", pos / 1000000.0 );
    pp->reset(); (*pp)();			// restore cursor position
    xunlock( &mutex );
    }
  }


#if defined __MSVCRT__
#include <windows.h>
#define _SC_NPROCESSORS_ONLN   1
#define _SC_THREAD_THREADS_MAX 2

long sysconf( int flag )
  {
  if( flag == _SC_NPROCESSORS_ONLN )
    {
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    return si.dwNumberOfProcessors;
    }
  if( flag != _SC_THREAD_THREADS_MAX ) errno = EINVAL;
  return -1;		// unlimited threads or error
  }

#endif	// __MSVCRT__


int main( const int argc, const char * const argv[] )
  {
  /* Mapping from gzip/bzip2 style 0..9 compression levels to the
     corresponding LZMA compression parameters. */
  const Lzma_options option_mapping[] =
    {
    {   65535,  16 },		// -0 (65535,16 chooses fast encoder)
    { 1 << 20,   5 },		// -1
    { 3 << 19,   6 },		// -2
    { 1 << 21,   8 },		// -3
    { 3 << 20,  12 },		// -4
    { 1 << 22,  20 },		// -5
    { 1 << 23,  36 },		// -6
    { 1 << 24,  68 },		// -7
    { 3 << 23, 132 },		// -8
    { 1 << 25, 273 } };		// -9
  Lzma_options encoder_options = option_mapping[6];	// default = "-6"
  std::string default_output_filename;
  int data_size = 0;
  int debug_level = 0;
  int num_workers = 0;		// start this many worker threads
  int in_slots = 4;
  int out_slots = 64;
  Mode program_mode = m_compress;
  Cl_options cl_opts;		// command-line options
  bool force = false;
  bool keep_input_files = false;
  bool recompress = false;
  bool to_stdout = false;
  if( argc > 0 ) invocation_name = argv[0];

  enum { opt_chk = 256, opt_dbg, opt_in, opt_lt, opt_out };
  const Arg_parser::Option options[] =
    {
    { '0', "fast",              Arg_parser::no  },
    { '1', 0,                   Arg_parser::no  },
    { '2', 0,                   Arg_parser::no  },
    { '3', 0,                   Arg_parser::no  },
    { '4', 0,                   Arg_parser::no  },
    { '5', 0,                   Arg_parser::no  },
    { '6', 0,                   Arg_parser::no  },
    { '7', 0,                   Arg_parser::no  },
    { '8', 0,                   Arg_parser::no  },
    { '9', "best",              Arg_parser::no  },
    { 'a', "trailing-error",    Arg_parser::no  },
    { 'b', "member-size",       Arg_parser::yes },
    { 'B', "data-size",         Arg_parser::yes },
    { 'c', "stdout",            Arg_parser::no  },
    { 'd', "decompress",        Arg_parser::no  },
    { 'f', "force",             Arg_parser::no  },
    { 'F', "recompress",        Arg_parser::no  },
    { 'h', "help",              Arg_parser::no  },
    { 'k', "keep",              Arg_parser::no  },
    { 'l', "list",              Arg_parser::no  },
    { 'm', "match-length",      Arg_parser::yes },
    { 'n', "threads",           Arg_parser::yes },
    { 'o', "output",            Arg_parser::yes },
    { 'q', "quiet",             Arg_parser::no  },
    { 's', "dictionary-size",   Arg_parser::yes },
    { 'S', "volume-size",       Arg_parser::yes },
    { 't', "test",              Arg_parser::no  },
    { 'v', "verbose",           Arg_parser::no  },
    { 'V', "version",           Arg_parser::no  },
    { opt_chk, "check-lib",     Arg_parser::no  },
    { opt_dbg, "debug",         Arg_parser::yes },
    { opt_in, "in-slots",       Arg_parser::yes },
    { opt_lt, "loose-trailing", Arg_parser::no  },
    { opt_out, "out-slots",     Arg_parser::yes },
    {  0, 0,                    Arg_parser::no  } };

  const Arg_parser parser( argc, argv, options );
  if( parser.error().size() )				// bad option
    { show_error( parser.error().c_str(), 0, true ); return 1; }

  const long num_online = std::max( 1L, sysconf( _SC_NPROCESSORS_ONLN ) );
  long max_workers = sysconf( _SC_THREAD_THREADS_MAX );
  if( max_workers < 1 || max_workers > INT_MAX / (int)sizeof (pthread_t) )
    max_workers = INT_MAX / sizeof (pthread_t);

  int argind = 0;
  for( ; argind < parser.arguments(); ++argind )
    {
    const int code = parser.code( argind );
    if( !code ) break;					// no more options
    const char * const pn = parser.parsed_name( argind ).c_str();
    const std::string & sarg = parser.argument( argind );
    const char * const arg = sarg.c_str();
    switch( code )
      {
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
                encoder_options = option_mapping[code-'0']; break;
      case 'a': cl_opts.ignore_trailing = false; break;
      case 'b': break;
      case 'B': data_size = getnum( arg, pn, 2 * LZ_min_dictionary_size(),
                                    2 * LZ_max_dictionary_size() ); break;
      case 'c': to_stdout = true; break;
      case 'd': set_mode( program_mode, m_decompress ); break;
      case 'f': force = true; break;
      case 'F': recompress = true; break;
      case 'h': show_help( num_online ); return 0;
      case 'k': keep_input_files = true; break;
      case 'l': set_mode( program_mode, m_list ); break;
      case 'm': encoder_options.match_len_limit =
                  getnum( arg, pn, LZ_min_match_len_limit(),
                                   LZ_max_match_len_limit() ); break;
      case 'n': num_workers = getnum( arg, pn, 1, max_workers ); break;
      case 'o': if( sarg == "-" ) to_stdout = true;
                else { default_output_filename = sarg; } break;
      case 'q': verbosity = -1; break;
      case 's': encoder_options.dictionary_size = get_dict_size( arg, pn );
                break;
      case 'S': break;
      case 't': set_mode( program_mode, m_test ); break;
      case 'v': if( verbosity < 4 ) ++verbosity; break;
      case 'V': show_version(); return 0;
      case opt_chk: return check_lib();
      case opt_dbg: debug_level = getnum( arg, pn, 0, 3 ); break;
      case opt_in: in_slots = getnum( arg, pn, 1, 64 ); break;
      case opt_lt: cl_opts.loose_trailing = true; break;
      case opt_out: out_slots = getnum( arg, pn, 1, 1024 ); break;
      default: internal_error( "uncaught option." );
      }
    } // end process options

  if( LZ_version()[0] < '1' )
    { show_error( "Wrong library version. At least lzlib 1.0 is required." );
      return 1; }

#if defined __MSVCRT__ || defined __OS2__
  setmode( STDIN_FILENO, O_BINARY );
  setmode( STDOUT_FILENO, O_BINARY );
#endif

  std::vector< std::string > filenames;
  bool filenames_given = false;
  for( ; argind < parser.arguments(); ++argind )
    {
    filenames.push_back( parser.argument( argind ) );
    if( filenames.back() != "-" ) filenames_given = true;
    }
  if( filenames.empty() ) filenames.push_back("-");

  if( program_mode == m_list ) return list_files( filenames, cl_opts );

  const bool fast = encoder_options.dictionary_size == 65535 &&
                    encoder_options.match_len_limit == 16;
  if( data_size <= 0 )
    {
    if( fast ) data_size = 1 << 20;
    else data_size = 2 * std::max( 65536, encoder_options.dictionary_size );
    }
  else if( !fast && data_size < encoder_options.dictionary_size )
    encoder_options.dictionary_size =
      std::max( data_size, LZ_min_dictionary_size() );

  if( num_workers <= 0 )
    {
    if( program_mode == m_compress && sizeof (void *) <= 4 )
      {
      // use less than 2.22 GiB on 32 bit systems
      const long long limit = ( 27LL << 25 ) + ( 11LL << 27 );	// 4 * 568 MiB
      const long long mem = ( 27LL * data_size ) / 8 +
        ( fast ? 3LL << 19 : 11LL * encoder_options.dictionary_size );
      const int nmax32 = std::max( limit / mem, 1LL );
      if( max_workers > nmax32 ) max_workers = nmax32;
      }
    num_workers = std::min( num_online, max_workers );
    }

  if( program_mode == m_test ) to_stdout = false;	// apply overrides
  if( program_mode == m_test || to_stdout ) default_output_filename.clear();

  if( to_stdout && program_mode != m_test )	// check tty only once
    { outfd = STDOUT_FILENO; if( !check_tty_out( program_mode ) ) return 1; }
  else outfd = -1;

  const bool to_file = !to_stdout && program_mode != m_test &&
                       default_output_filename.size();
  if( !to_stdout && program_mode != m_test && ( filenames_given || to_file ) )
    set_signals( signal_handler );

  Pretty_print pp( filenames );

  int failed_tests = 0;
  int retval = 0;
  const bool one_to_one = !to_stdout && program_mode != m_test && !to_file;
  bool stdin_used = false;
  struct stat in_stats;
  for( unsigned i = 0; i < filenames.size(); ++i )
    {
    std::string input_filename;
    int infd;

    pp.set_name( filenames[i] );
    if( filenames[i] == "-" )
      {
      if( stdin_used ) continue; else stdin_used = true;
      infd = STDIN_FILENO;
      if( !check_tty_in( pp.name(), infd, program_mode, retval ) ) continue;
      if( one_to_one ) { outfd = STDOUT_FILENO; output_filename.clear(); }
      }
    else
      {
      const int eindex = extension_index( input_filename = filenames[i] );
      infd = open_instream2( input_filename.c_str(), &in_stats, program_mode,
                             eindex, one_to_one, recompress );
      if( infd < 0 ) { set_retval( retval, 1 ); continue; }
      if( !check_tty_in( pp.name(), infd, program_mode, retval ) ) continue;
      if( one_to_one )			// open outfd after checking infd
        {
        if( program_mode == m_compress )
          set_c_outname( input_filename, true, true );
        else set_d_outname( input_filename, eindex );
        if( !open_outstream( force, true ) )
          { close( infd ); set_retval( retval, 1 ); continue; }
        }
      }

    if( one_to_one && !check_tty_out( program_mode ) )
      { set_retval( retval, 1 ); return retval; }	// don't delete a tty

    if( to_file && outfd < 0 )		// open outfd after checking infd
      {
      if( program_mode == m_compress ) set_c_outname( default_output_filename,
                                       filenames_given, false );
      else output_filename = default_output_filename;
      if( !open_outstream( force, false ) || !check_tty_out( program_mode ) )
        return 1;	// check tty only once and don't try to delete a tty
      }

    const struct stat * const in_statsp =
      ( input_filename.size() && one_to_one ) ? &in_stats : 0;
    const bool infd_isreg = input_filename.size() && S_ISREG( in_stats.st_mode );
    const unsigned long long cfile_size =
      infd_isreg ? ( in_stats.st_size + 99 ) / 100 : 0;
    int tmp;
    if( program_mode == m_compress )
      tmp = compress( cfile_size, data_size, encoder_options.dictionary_size,
                      encoder_options.match_len_limit, num_workers,
                      infd, outfd, pp, debug_level );
    else
      tmp = decompress( cfile_size, num_workers, infd, outfd, cl_opts, pp,
                        debug_level, in_slots, out_slots, infd_isreg,
                        one_to_one );
    if( close( infd ) != 0 )
      { show_file_error( pp.name(), "Error closing input file", errno );
        set_retval( tmp, 1 ); }
    set_retval( retval, tmp );
    if( tmp )
      { if( program_mode != m_test ) cleanup_and_fail( retval );
        else ++failed_tests; }

    if( delete_output_on_interrupt && one_to_one )
      close_and_set_permissions( in_statsp );
    if( input_filename.size() && !keep_input_files && one_to_one )
      std::remove( input_filename.c_str() );
    }
  if( delete_output_on_interrupt )					// -o
    close_and_set_permissions( ( retval == 0 && !stdin_used &&
      filenames_given && filenames.size() == 1 ) ? &in_stats : 0 );
  else if( outfd >= 0 && close( outfd ) != 0 )				// -c
    {
    show_error( "Error closing stdout", errno );
    set_retval( retval, 1 );
    }
  if( failed_tests > 0 && verbosity >= 1 && filenames.size() > 1 )
    std::fprintf( stderr, "%s: warning: %d %s failed the test.\n",
                  program_name, failed_tests,
                  ( failed_tests == 1 ) ? "file" : "files" );
  return retval;
  }
