/* Plzip - Massively parallel implementation of lzip
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

#include <pthread.h>

enum {
  min_dictionary_bits = 12,
  min_dictionary_size = 1 << min_dictionary_bits,	// >= modeled_distances
  max_dictionary_bits = 29,
  max_dictionary_size = 1 << max_dictionary_bits,
  min_member_size = 36 };


// defined in main.cc
extern int verbosity;

class Pretty_print		// requires global var 'int verbosity'
  {
  std::string name_;
  std::string padded_name;
  const char * const stdin_name;
  unsigned longest_name;
  mutable bool first_post;

public:
  Pretty_print( const std::vector< std::string > & filenames )
    : stdin_name( "(stdin)" ), longest_name( 0 ), first_post( false )
    {
    if( verbosity <= 0 ) return;
    const unsigned stdin_name_len = std::strlen( stdin_name );
    for( unsigned i = 0; i < filenames.size(); ++i )
      {
      const std::string & s = filenames[i];
      const unsigned len = ( s == "-" ) ? stdin_name_len : s.size();
      if( longest_name < len ) longest_name = len;
      }
    if( longest_name == 0 ) longest_name = stdin_name_len;
    }

  void set_name( const std::string & filename )
    {
    if( filename.size() && filename != "-" ) name_ = filename;
    else name_ = stdin_name;
    padded_name = "  "; padded_name += name_; padded_name += ": ";
    if( longest_name > name_.size() )
      padded_name.append( longest_name - name_.size(), ' ' );
    first_post = true;
    }

  void reset() const { if( name_.size() ) first_post = true; }
  const char * name() const { return name_.c_str(); }
  void operator()( const char * const msg = 0 ) const;
  };


inline bool isvalid_ds( const unsigned dictionary_size )
  { return dictionary_size >= min_dictionary_size &&
           dictionary_size <= max_dictionary_size; }


inline int real_bits( unsigned value )
  {
  int bits = 0;
  while( value > 0 ) { value >>= 1; ++bits; }
  return bits;
  }


const uint8_t lzip_magic[4] = { 0x4C, 0x5A, 0x49, 0x50 };	// "LZIP"

struct Lzip_header
  {
  enum { size = 6 };
  uint8_t data[size];			// 0-3 magic bytes
					//   4 version
					//   5 coded dictionary size

  void set_magic() { std::memcpy( data, lzip_magic, 4 ); data[4] = 1; }
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

  bool dictionary_size( const unsigned sz )
    {
    if( !isvalid_ds( sz ) ) return false;
    data[5] = real_bits( sz - 1 );
    if( sz > min_dictionary_size )
      {
      const unsigned base_size = 1 << data[5];
      const unsigned fraction = base_size / 16;
      for( unsigned i = 7; i >= 1; --i )
        if( base_size - ( i * fraction ) >= sz )
          { data[5] |= i << 5; break; }
      }
    return true;
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

  void data_crc( unsigned crc )
    { for( int i = 0; i <= 3; ++i ) { data[i] = (uint8_t)crc; crc >>= 8; } }

  unsigned long long data_size() const
    {
    unsigned long long tmp = 0;
    for( int i = 11; i >= 4; --i ) { tmp <<= 8; tmp += data[i]; }
    return tmp;
    }

  void data_size( unsigned long long sz )
    { for( int i = 4; i <= 11; ++i ) { data[i] = (uint8_t)sz; sz >>= 8; } }

  unsigned long long member_size() const
    {
    unsigned long long tmp = 0;
    for( int i = 19; i >= 12; --i ) { tmp <<= 8; tmp += data[i]; }
    return tmp;
    }

  void member_size( unsigned long long sz )
    { for( int i = 12; i <= 19; ++i ) { data[i] = (uint8_t)sz; sz >>= 8; } }

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


struct Cl_options		// command-line options
  {
  bool ignore_trailing;
  bool loose_trailing;

  Cl_options() : ignore_trailing( true ), loose_trailing( false ) {}
  };


inline void set_retval( int & retval, const int new_val )
  { if( retval < new_val ) retval = new_val; }

const char * const bad_magic_msg = "Bad magic number (file not in lzip format).";
const char * const bad_dict_msg = "Invalid dictionary size in member header.";
const char * const corrupt_mm_msg = "Corrupt header in multimember file.";
const char * const trailing_msg = "Trailing data not allowed.";
const char * const mem_msg = "Not enough memory.";

// defined in compress.cc
int readblock( const int fd, uint8_t * const buf, const int size );
int writeblock( const int fd, const uint8_t * const buf, const int size );
void xinit_mutex( pthread_mutex_t * const mutex );
void xinit_cond( pthread_cond_t * const cond );
void xdestroy_mutex( pthread_mutex_t * const mutex );
void xdestroy_cond( pthread_cond_t * const cond );
void xlock( pthread_mutex_t * const mutex );
void xunlock( pthread_mutex_t * const mutex );
void xwait( pthread_cond_t * const cond, pthread_mutex_t * const mutex );
void xsignal( pthread_cond_t * const cond );
void xbroadcast( pthread_cond_t * const cond );
int compress( const unsigned long long cfile_size,
              const int data_size, const int dictionary_size,
              const int match_len_limit, const int num_workers,
              const int infd, const int outfd,
              const Pretty_print & pp, const int debug_level );

// defined in lzip_index.cc
class Lzip_index;				// forward declaration

// defined in dec_stdout.cc
int dec_stdout( const int num_workers, const int infd, const int outfd,
                const Pretty_print & pp, const int debug_level,
                const int out_slots, const Lzip_index & lzip_index );

// defined in dec_stream.cc
int dec_stream( const unsigned long long cfile_size, const int num_workers,
                const int infd, const int outfd, const Cl_options & cl_opts,
                const Pretty_print & pp, const int debug_level,
                const int in_slots, const int out_slots );

// defined in decompress.cc
int preadblock( const int fd, uint8_t * const buf, const int size,
                const long long pos );
class Shared_retval;
void decompress_error( struct LZ_Decoder * const decoder,
                       const Pretty_print & pp,
                       Shared_retval & shared_retval, const int worker_id );
void show_results( const unsigned long long in_size,
                   const unsigned long long out_size,
                   const unsigned dictionary_size, const bool testing );
int decompress( const unsigned long long cfile_size, int num_workers,
                const int infd, const int outfd, const Cl_options & cl_opts,
                const Pretty_print & pp, const int debug_level,
                const int in_slots, const int out_slots,
                const bool infd_isreg, const bool one_to_one );

// defined in list.cc
int list_files( const std::vector< std::string > & filenames,
                const Cl_options & cl_opts );

// defined in main.cc
struct stat;
const char * bad_version( const unsigned version );
const char * format_ds( const unsigned dictionary_size );
void show_header( const unsigned dictionary_size );
int open_instream( const char * const name, struct stat * const in_statsp,
                   const bool one_to_one, const bool reg_only = false );
void cleanup_and_fail( const int retval = 1 );	// terminate the program
void show_error( const char * const msg, const int errcode = 0,
                 const bool help = false );
void show_file_error( const char * const filename, const char * const msg,
                      const int errcode = 0 );
void internal_error( const char * const msg );
void show_progress( const unsigned long long packet_size,
                    const unsigned long long cfile_size = 0,
                    const Pretty_print * const p = 0 );


class Slot_tally
  {
  const int num_slots;				// total slots
  int num_free;					// remaining free slots
  pthread_mutex_t mutex;
  pthread_cond_t slot_av;			// slot available

  Slot_tally( const Slot_tally & );		// declared as private
  void operator=( const Slot_tally & );		// declared as private

public:
  explicit Slot_tally( const int slots )
    : num_slots( slots ), num_free( slots )
    { xinit_mutex( &mutex ); xinit_cond( &slot_av ); }

  ~Slot_tally() { xdestroy_cond( &slot_av ); xdestroy_mutex( &mutex ); }

  bool all_free() { return num_free == num_slots; }

  void get_slot()				// wait for a free slot
    {
    xlock( &mutex );
    while( num_free <= 0 ) xwait( &slot_av, &mutex );
    --num_free;
    xunlock( &mutex );
    }

  void leave_slot()				// return a slot to the tally
    {
    xlock( &mutex );
    if( ++num_free == 1 ) xsignal( &slot_av );	// num_free was 0
    xunlock( &mutex );
    }
  };


class Shared_retval		// shared return value protected by a mutex
  {
  int retval;
  pthread_mutex_t mutex;

  Shared_retval( const Shared_retval & );	// declared as private
  void operator=( const Shared_retval & );	// declared as private

public:
  Shared_retval() : retval( 0 ) { xinit_mutex( &mutex ); }

  bool set_value( const int val )	// only one thread can set retval > 0
    {					// (and print an error message)
    xlock( &mutex );
    const bool done = ( retval == 0 && val > 0 );
    if( done ) retval = val;
    xunlock( &mutex );
    return done;
    }

  int operator()() const { return retval; }
  };
