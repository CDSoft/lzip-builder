#include <sys/stat.h>
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

inline bool data_may_follow( const Typeflag typeflag )
  { return typeflag == tf_regular || typeflag == tf_hiperf; }

inline bool uid_gid_in_range( const long long uid, const long long gid )
  { return uid == (long long)( (uid_t)uid ) &&
           gid == (long long)( (gid_t)gid ); }

const char * const dotdot_msg = "Contains a '..' component, skipping.";
const char * const cantln_msg = "Can't %slink '%s' to '%s'";
const char * const mkdir_msg = "Can't create directory";
const char * const mknod_msg = "Can't create device node";
const char * const mkfifo_msg = "Can't create FIFO file";
const char * const uftype_msg = "%s: Unknown file type 0x%02X, skipping.";
const char * const chown_msg = "Can't change file owner";

mode_t get_umask();

struct Chdir_error {};


class T_names		// list of names in the argument of an option '-T'
  {
  char * buffer;			// max 4 GiB for the whole -T file
  long file_size;			// 0 for empty file
  std::vector< uint32_t > name_idx;	// for each name in buffer
  std::vector< uint8_t > name_pending_;	// 'uint8_t' for concurrent update

public:
  explicit T_names( const char * const filename );
  ~T_names() { if( buffer ) std::free( buffer ); buffer = 0; file_size = 0; }

  unsigned names() const { return name_idx.size(); }
  const char * name( const unsigned i ) const { return buffer + name_idx[i]; }
  bool name_pending( const unsigned i ) const { return name_pending_[i]; }
  void reset_name_pending( const unsigned i ) { name_pending_[i] = false; }
  };


/* Lists of file names to be compared, deleted, extracted, or listed.
   name_pending_or_idx uses uint8_t instead of bool to allow concurrent
   update and provide space for 256 '-T' options. */
struct Cl_names
  {
  // if parser.code( i ) == 'T', name_pending_or_idx[i] is the index in t_vec
  std::vector< uint8_t > name_pending_or_idx;
  std::vector< T_names * > t_vec;

  explicit Cl_names( const Arg_parser & parser );
  ~Cl_names() { for( unsigned i = 0; i < t_vec.size(); ++i ) delete t_vec[i]; }

  T_names & t_names( const unsigned i )
    { return *t_vec[name_pending_or_idx[i]]; }
  bool names_remain( const Arg_parser & parser ) const;
  };


// defined in common_decode.cc
bool check_skip_filename( const Cl_options & cl_opts, Cl_names & cl_names,
                          const char * const filename, const int cwd_fd = -1,
                          std::string * const msgp = 0 );
bool format_member_name( const Extended & extended, const Tar_header header,
                         Resizable_buffer & rbuf, const bool long_format );
bool show_member_name( const Extended & extended, const Tar_header header,
                       const int vlevel, Resizable_buffer & rbuf );

// defined in decode_lz.cc
struct Archive_descriptor;			// forward declaration
int decode_lz( const Cl_options & cl_opts, const Archive_descriptor & ad,
               Cl_names & cl_names );

// defined in delete.cc
bool safe_seek( const int fd, const long long pos );
int tail_copy( const Arg_parser & parser, const Archive_descriptor & ad,
               Cl_names & cl_names, const long long istream_pos,
               const int outfd, int retval );

// defined in delete_lz.cc
int delete_members_lz( const Cl_options & cl_opts,
                       const Archive_descriptor & ad,
                       Cl_names & cl_names, const int outfd );
