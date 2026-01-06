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

#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <stdint.h>

enum { header_size = 512 };
typedef uint8_t Tar_header[header_size];

enum Offsets {
  name_o = 0, mode_o = 100, uid_o = 108, gid_o = 116, size_o = 124,
  mtime_o = 136, chksum_o = 148, typeflag_o = 156, linkname_o = 157,
  magic_o = 257, version_o = 263, uname_o = 265, gname_o = 297,
  devmajor_o = 329, devminor_o = 337, prefix_o = 345 };

enum Lengths {
  name_l = 100, mode_l = 8, uid_l = 8, gid_l = 8, size_l = 12,
  mtime_l = 12, chksum_l = 8, typeflag_l = 1, linkname_l = 100,
  magic_l = 6, version_l = 2, uname_l = 32, gname_l = 32,
  devmajor_l = 8, devminor_l = 8, prefix_l = 155 };

enum Typeflag {
  tf_regular = '0', tf_link = '1', tf_symlink = '2', tf_chardev = '3',
  tf_blockdev = '4', tf_directory = '5', tf_fifo = '6', tf_hiperf = '7',
  tf_global = 'g', tf_extended = 'x' };

const uint8_t ustar_magic[magic_l] =
  { 0x75, 0x73, 0x74, 0x61, 0x72, 0 };			// "ustar\0"

inline bool check_ustar_magic( const Tar_header header )
  { return std::memcmp( header + magic_o, ustar_magic, magic_l ) == 0; }

inline void init_tar_header( Tar_header header )    // set magic and version
  {
  std::memset( header, 0, header_size );
  std::memcpy( header + magic_o, ustar_magic, magic_l - 1 );
  header[version_o] = header[version_o+1] = '0';
  }

inline void print_octal( uint8_t * const buf, int size, unsigned long long num )
  { while( --size >= 0 ) { buf[size] = num % 8 + '0'; num /= 8; } }


// Round "size" to the next multiple of header size (512).
//
inline unsigned long long round_up( const unsigned long long size )
  {
  const unsigned padding = ( header_size - size % header_size ) % header_size;
  return size + padding;
  }


inline int decimal_digits( unsigned long long value )
  {
  int digits = 1;
  while( value >= 10 ) { value /= 10; ++digits; }
  return digits;
  }


inline bool dotdot_at_i( const char * const filename, const int i )
  {
  return filename[i] == '.' && filename[i+1] == '.' &&
         ( i == 0 || filename[i-1] == '/' ) &&
         ( filename[i+2] == 0 || filename[i+2] == '/' );
  }


inline bool contains_dotdot( const char * const filename )
  {
  for( int i = 0; filename[i]; ++i )
    if( dotdot_at_i( filename, i ) ) return true;
  return false;
  }


class Resizable_buffer
  {
  char * p;
  unsigned long size_;			// size_ < LONG_MAX

public:
  // must be >= 87 for format_member_name
  enum { default_initial_size = 2 * header_size };

  explicit Resizable_buffer( const unsigned long initial_size =
                             default_initial_size )
    : p( (char *)std::malloc( initial_size ) ), size_( p ? initial_size : 0 ) {}
  ~Resizable_buffer() { if( p ) std::free( p ); p = 0; size_ = 0; }

  bool resize( const unsigned long long new_size )
    {
    if( new_size >= LONG_MAX ) return false;
    if( size_ < new_size )
      {
      char * const tmp = (char *)std::realloc( p, new_size );
      if( !tmp ) return false;
      p = tmp; size_ = new_size;
      }
    return true;
    }
  char * operator()() { return p; }		// no need for operator[]
  const char * operator()() const { return p; }
  uint8_t * u8() { return (uint8_t *)p; }
  const uint8_t * u8() const { return (const uint8_t *)p; }
  unsigned long size() const { return size_; }
  };


inline bool uid_in_ustar_range( const long long uid )	// also for gid
  { return uid >= 0 && uid < 1 << 21; }

inline bool time_in_ustar_range( const long long seconds )
  { return seconds >= 0 && seconds < 1LL << 33; }


/* The sign of the seconds field applies to the whole time value.
   A nanoseconds value out of range means an invalid time. */
class Etime				// time since (or before) the epoch
  {
  long long sec_;
  int nsec_;				// range [0, 999_999_999]

public:
  Etime() : sec_( 0 ), nsec_( -1 ) {}
  void reset() { sec_ = 0; nsec_ = -1; }
  void set( const long long s ) { sec_ = s; nsec_ = 0; }
  long long sec() const { return sec_; }
  int nsec() const { return nsec_; }
  bool isvalid() const { return nsec_ >= 0 && nsec_ <= 999999999; }
  bool out_of_ustar_range() const
    { return isvalid() && !time_in_ustar_range( sec_ ); }

  unsigned decimal_size() const;
  unsigned print( char * const buf ) const;
  bool parse( const char * const ptr, const char ** const tailp,
              const int size );
  };


class Extended			// stores metadata from/for extended records
  {
  static std::vector< std::string > unknown_keywords;	// already diagnosed
  std::string linkpath_;		// these are the real metadata
  std::string path_;
  long long file_size_;			// >= 0 && <= max_file_size
  long long uid_, gid_;			// may not fit in unsigned int
  Etime atime_, mtime_;

  // cached sizes; if full_size_ <= -4 they must be recalculated
  mutable int edsize_;			// extended data size
  mutable int padded_edsize_;		// edsize rounded up
  mutable int full_size_;		// header + padded edsize
  mutable int linkpath_recsize_;
  mutable int path_recsize_;
  mutable int file_size_recsize_;
  mutable int uid_recsize_;
  mutable int gid_recsize_;
  mutable int atime_recsize_;
  mutable int mtime_recsize_;

  // true if CRC present in parsed or formatted records
  mutable bool crc_present_;

  void calculate_sizes() const;
  void unknown_keyword( const char * const buf, const int size,
                        std::vector< std::string > * const msg_vecp = 0 ) const;

public:
  enum { max_edata_size = ( 1 << 21 ) * header_size };	// 1 GiB
  enum { max_file_size = LLONG_MAX - header_size };	// for padding
  static const std::string crc_record;
  std::string removed_prefix;

  Extended()
    : file_size_( 0 ), uid_( -1 ), gid_( -1 ), edsize_( 0 ),
      padded_edsize_( 0 ), full_size_( 0 ), linkpath_recsize_( 0 ),
      path_recsize_( 0 ), file_size_recsize_( 0 ), uid_recsize_( 0 ),
      gid_recsize_( 0 ), atime_recsize_( 0 ), mtime_recsize_( 0 ),
      crc_present_( false ) {}

  void reset()
    { linkpath_.clear(); path_.clear(); file_size_ = 0; uid_ = -1; gid_ = -1;
      atime_.reset(); mtime_.reset(); edsize_ = 0; padded_edsize_ = 0;
      full_size_ = 0; linkpath_recsize_ = 0; path_recsize_ = 0;
      file_size_recsize_ = 0; uid_recsize_ = 0; gid_recsize_ = 0;
      atime_recsize_ = 0; mtime_recsize_ = 0; crc_present_ = false;
      removed_prefix.clear(); }

  const std::string & linkpath() const { return linkpath_; }
  const std::string & path() const { return path_; }
  long long file_size() const { return file_size_; }
  long long get_file_size_and_reset( const Tar_header header );
  long long get_uid() const { return uid_; }
  long long get_gid() const { return gid_; }
  const Etime & atime() const { return atime_; }
  const Etime & mtime() const { return mtime_; }

  void linkpath( const char * const lp ) { linkpath_ = lp; full_size_ = -4; }
  void path( const char * const p ) { path_ = p; full_size_ = -4; }
  void file_size( const long long fs ) { full_size_ = -4;
    file_size_ = ( fs >= 0 && fs <= max_file_size ) ? fs : 0; }
  bool set_uid( const long long id )
    { if( id >= 0 ) { uid_ = id; full_size_ = -4; } return id >= 0; }
  bool set_gid( const long long id )
    { if( id >= 0 ) { gid_ = id; full_size_ = -4; } return id >= 0; }
  void set_atime( const long long s ) { atime_.set( s ); full_size_ = -4; }
  void set_mtime( const long long s ) { mtime_.set( s ); full_size_ = -4; }

  /* Return the size of the extended block, or 0 if empty.
     Return -1 if error, -2 if out of memory, -3 if block too long. */
  int full_size() const
    { if( full_size_ <= -4 ) calculate_sizes(); return full_size_; }
  int format_block( Resizable_buffer & rbuf ) const;
  const char * full_size_error() const;

  bool crc_present() const { return crc_present_; }
  bool parse( const char * const buf, const int edsize,
              const bool permissive,
              std::vector< std::string > * const msg_vecp = 0 );
  void fill_from_ustar( const Tar_header header );
  };


class CRC32
  {
  uint32_t data[256];		// Table of CRCs of all 8-bit messages.

public:
  explicit CRC32( const bool castagnoli = false )
    {
    const unsigned cpol = 0x82F63B78U;	// CRC32-C  Castagnoli polynomial
    const unsigned ipol = 0xEDB88320U;	// IEEE 802.3 Ethernet polynomial
    const unsigned poly = castagnoli ? cpol : ipol;

    for( unsigned n = 0; n < 256; ++n )
      {
      unsigned c = n;
      for( int k = 0; k < 8; ++k )
        { if( c & 1 ) c = poly ^ ( c >> 1 ); else c >>= 1; }
      data[n] = c;
      }
    }

  void update_byte( uint32_t & crc, const uint8_t byte ) const
    { crc = data[(crc^byte)&0xFF] ^ ( crc >> 8 ); }

  // about as fast as it is possible without messing with endianness
  void update_buf( uint32_t & crc, const uint8_t * const buffer,
                   const int size ) const
    {
    uint32_t c = crc;
    for( int i = 0; i < size; ++i )
      c = data[(c^buffer[i])&0xFF] ^ ( c >> 8 );
    crc = c;
    }

  uint32_t compute_crc( const uint8_t * const buffer, const int size ) const
    {
    uint32_t crc = 0xFFFFFFFFU;
    update_buf( crc, buffer, size );
    return crc ^ 0xFFFFFFFFU;
    }

  // compute the crc of size bytes except a window of 8 bytes at pos
  uint32_t windowed_crc( const uint8_t * const buffer, const int pos,
                         const int size ) const
    {
    uint32_t crc = 0xFFFFFFFFU;
    update_buf( crc, buffer, pos );
    update_buf( crc, buffer + pos + 8, size - pos - 8 );
    return crc ^ 0xFFFFFFFFU;
    }
  };


struct Lzma_options
  {
  int dictionary_size;		// 4 KiB .. 512 MiB
  int match_len_limit;		// 5 .. 273
  };
const Lzma_options option_mapping[] =
  {
  {   65535,  16 },		// -0
  { 1 << 20,   5 },		// -1
  { 3 << 19,   6 },		// -2
  { 1 << 21,   8 },		// -3
  { 3 << 20,  12 },		// -4
  { 1 << 22,  20 },		// -5
  { 1 << 23,  36 },		// -6
  { 1 << 24,  68 },		// -7
  { 3 << 23, 132 },		// -8
  { 1 << 25, 273 } };		// -9


enum {
  min_dictionary_bits = 12,
  min_dictionary_size = 1 << min_dictionary_bits,
  max_dictionary_bits = 29,
  max_dictionary_size = 1 << max_dictionary_bits,
  min_member_size = 36,
  min_data_size = 2 * min_dictionary_size,
  max_data_size = 2 * max_dictionary_size };


inline bool isvalid_ds( const unsigned dictionary_size )
  { return dictionary_size >= min_dictionary_size &&
           dictionary_size <= max_dictionary_size; }


const uint8_t lzip_magic[4] = { 0x4C, 0x5A, 0x49, 0x50 };	// "LZIP"

struct Lzip_header
  {
  enum { size = 6 };
  uint8_t data[size];			// 0-3 magic bytes
					//   4 version
					//   5 coded dictionary size

  bool check_magic() const { return std::memcmp( data, lzip_magic, 4 ) == 0; }

  bool check_prefix( const int sz ) const	// detect (truncated) header
    {
    for( int i = 0; i < sz && i < 4; ++i )
      if( data[i] != lzip_magic[i] ) return false;
    return sz > 0;
    }

  bool check_corrupt() const			// detect corrupt header
    {
    int matches = 0;
    for( int i = 0; i < 4; ++i )
      if( data[i] == lzip_magic[i] ) ++matches;
    return matches > 1 && matches < 4;
    }

  uint8_t version() const { return data[4]; }
  bool check_version() const { return data[4] == 1; }

  unsigned dictionary_size() const
    {
    unsigned sz = 1 << ( data[5] & 0x1F );
    if( sz > min_dictionary_size )
      sz -= ( sz / 16 ) * ( ( data[5] >> 5 ) & 7 );
    return sz;
    }

  bool check() const
    { return check_magic() && check_version() &&
             isvalid_ds( dictionary_size() ); }
  };


struct Lzip_trailer
  {
  enum { size = 20 };
  uint8_t data[size];	//  0-3  CRC32 of the uncompressed data
			//  4-11 size of the uncompressed data
			// 12-19 member size including header and trailer

  unsigned data_crc() const
    {
    unsigned tmp = 0;
    for( int i = 3; i >= 0; --i ) { tmp <<= 8; tmp += data[i]; }
    return tmp;
    }

  unsigned long long data_size() const
    {
    unsigned long long tmp = 0;
    for( int i = 11; i >= 4; --i ) { tmp <<= 8; tmp += data[i]; }
    return tmp;
    }

  unsigned long long member_size() const
    {
    unsigned long long tmp = 0;
    for( int i = 19; i >= 12; --i ) { tmp <<= 8; tmp += data[i]; }
    return tmp;
    }

  bool check_consistency() const	// check internal consistency
    {
    const unsigned crc = data_crc();
    const unsigned long long dsize = data_size();
    if( ( crc == 0 ) != ( dsize == 0 ) ) return false;
    const unsigned long long msize = member_size();
    if( msize < min_member_size ) return false;
    const unsigned long long mlimit = ( 9 * dsize + 7 ) / 8 + min_member_size;
    if( mlimit > dsize && msize > mlimit ) return false;
    const unsigned long long dlimit = 7090 * ( msize - 26 ) - 1;
    if( dlimit > msize && dsize > dlimit ) return false;
    return true;
    }
  };


enum Program_mode { m_none, m_append, m_compress, m_concatenate, m_create,
                    m_delete, m_diff, m_extract, m_list };
enum Solidity { no_solid, bsolid, dsolid, asolid, solid };
class Arg_parser;

struct Cl_options		// command-line options
  {
  const Arg_parser & parser;
  std::string archive_name;
  std::string output_filename;
  long long mtime;
  long long uid;
  long long gid;
  Program_mode program_mode;
  Solidity solidity;
  int data_size;
  int debug_level;
  int level;			// compression level, < 0 means uncompressed
  unsigned num_files;		// number of files given in the command line
  int num_workers;		// start this many worker threads
  int out_slots;
  bool depth;
  bool dereference;
  bool ignore_ids;
  bool ignore_metadata;
  bool ignore_overflow;
  bool keep_damaged;
  bool level_set;		// compression level set in command line
  bool missing_crc;
  bool mount;
  bool mtime_set;
  bool numeric_owner;
  bool option_C_present;
  bool option_T_present;
  bool parallel;
  bool permissive;
  bool preserve_permissions;
  bool recursive;
  bool warn_newer;
  bool xdev;

  Cl_options( const Arg_parser & ap )
    : parser( ap ), mtime( 0 ), uid( -1 ), gid( -1 ), program_mode( m_none ),
      solidity( bsolid ), data_size( 0 ), debug_level( 0 ), level( 6 ),
      num_files( 0 ), num_workers( -1 ), out_slots( 64 ), depth( false ),
      dereference( false ), ignore_ids( false ), ignore_metadata( false ),
      ignore_overflow( false ), keep_damaged( false ), level_set( false ),
      missing_crc( false ), mount( false ), mtime_set( false ),
      numeric_owner( false ), option_C_present( false ),
      option_T_present( false ), parallel( false ), permissive( false ),
      preserve_permissions( false ), recursive( true ), warn_newer( false ),
      xdev( false ) {}

  void set_level( const int l ) { level = l; level_set = true; }

  int compressed() const;		// tri-state bool with error (-2)
  bool uncompressed() const { return level < 0 || level > 9; }
  bool to_stdout() const { return output_filename == "-"; }
  };

inline void set_retval( int & retval, const int new_val )
  { if( retval < new_val ) retval = new_val; }

const char * const bad_magic_msg = "Bad magic number (file not in lzip format).";
const char * const bad_dict_msg = "Invalid dictionary size in member header.";
const char * const corrupt_mm_msg = "Corrupt header in multimember file.";
const char * const bad_hdr_msg = "Corrupt or invalid tar header.";
const char * const gblrec_msg = "Error in global extended records.";
const char * const extrec_msg = "Error in extended records.";
const char * const miscrc_msg = "Missing CRC in extended records.";
const char * const misrec_msg = "Missing extended records.";
const char * const longrec_msg = "Extended records are too long.";
const char * const large_file_msg = "Input file is too large.";
const char * const end_msg = "Archive ends unexpectedly.";
const char * const mem_msg = "Not enough memory.";
const char * const mem_msg2 = "Not enough memory. Try a lower compression level.";
const char * const fv_msg1 = "Format violation: extended header followed by EOA blocks.";
const char * const fv_msg2 = "Format violation: extended header followed by global header.";
const char * const fv_msg3 = "Format violation: consecutive extended headers found.";
const char * const posix_msg = "This does not look like a POSIX tar archive.";
const char * const posix_lz_msg = "This does not look like a POSIX tar.lz archive.";
const char * const eclosa_msg = "Error closing archive";
const char * const eclosf_msg = "Error closing file";
const char * const rd_open_msg = "Can't open for reading";
const char * const rd_err_msg = "Read error";
const char * const wr_err_msg = "Write error";
const char * const seek_msg = "Seek error";
const char * const chdir_msg = "Error changing working directory";
const char * const intdir_msg = "Failed to create intermediate directory";

// defined in common.cc
unsigned long long parse_octal( const uint8_t * const ptr, const int size );
long readblock( const int fd, uint8_t * const buf, const long size );
int writeblock( const int fd, const uint8_t * const buf, const int size );

// defined in common_decode.cc
bool block_is_zero( const uint8_t * const buf, const int size );
bool make_dirs( const std::string & name );

// defined in common_mutex.cc
void exit_fail_mt( const int retval = 1 );	// terminate the program
bool print_removed_prefix( const std::string & prefix,
                           std::string * const msgp = 0 );
void set_error_status( const int retval );
int final_exit_status( int retval, const bool show_msg = true );

// defined in compress.cc
void show_atpos_error( const char * const filename, const long long pos,
                       const bool isarchive );
int compress( const Cl_options & cl_opts );

// defined in create.cc
bool copy_file( const int infd, const int outfd, const char * const filename,
                const long long max_size = -1 );
bool writeblock_wrapper( const int outfd, const uint8_t * const buffer,
                         const int size );
bool write_eoa_records( const int outfd, const bool compressed );
const char * remove_leading_dotslash( const char * const filename,
             std::string * const removed_prefixp, const bool dotdot = false );
bool fill_headers( std::string & estr, const char * const filename,
                   Extended & extended, Tar_header header,
                   long long & file_size, const int flag );
bool block_is_full( const int extended_size,
                    const unsigned long long file_size,
                    const unsigned long long target_size,
                    unsigned long long & partial_data_size );
unsigned ustar_chksum( const Tar_header header );
bool check_ustar_chksum( const Tar_header header );
bool has_lz_ext( const std::string & name );
int concatenate( const Cl_options & cl_opts );
int encode( const Cl_options & cl_opts );

// defined in decode.cc
bool compare_file_type( std::string & estr, std::string & ostr,
                        const Cl_options & cl_opts,
                        const Extended & extended, const Tar_header header );
class Archive_reader_base;
bool compare_file_contents( std::string & estr, std::string & ostr,
                            Archive_reader_base & ar, const long long file_size,
                            const char * const filename, const int infd2 );
int decode( const Cl_options & cl_opts );

// defined in delete.cc
int delete_members( const Cl_options & cl_opts );

// defined in exclude.cc
namespace Exclude {
void add_pattern( const std::string & arg );
bool excluded( const char * const filename );
} // end namespace Exclude

// defined in extended.cc
extern const CRC32 crc32c;

// defined in lzip_index.cc
int seek_read( const int fd, uint8_t * const buf, const int size,
               const long long pos );

// defined in main.cc
extern int verbosity;
extern const char * const program_name;
struct stat;
int hstat( const char * const filename, struct stat * const st,
           const bool dereference );
bool nonempty_arg( const Arg_parser & parser, const int i );
const char * format_num3( unsigned long long num, const bool negative = false );
int open_instream( const char * const name, struct stat * const in_statsp = 0 );
int open_outstream( const std::string & name, const bool create = true,
                    Resizable_buffer * const rbufp = 0, const bool force = true );
void show_error( const char * const msg, const int errcode = 0,
                 const bool help = false );
bool format_error( Resizable_buffer & rbuf, const int errcode,
                   const char * const format, ... );
bool format_error( std::string & msg, const int errcode,
                   const char * const format, ... );
void print_error( const int errcode, const char * const format, ... );
void format_file_error( std::string & estr, const char * const filename,
                        const char * const msg, const int errcode = 0 );
bool format_file_error( Resizable_buffer & rbuf, const char * const filename,
                        const char * const msg, const int errcode = 0 );
void show_file_error( const char * const filename, const char * const msg,
                      const int errcode = 0 );
void internal_error( const char * const msg );
