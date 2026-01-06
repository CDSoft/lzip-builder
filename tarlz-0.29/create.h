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

class Archive_attrs
  {
  struct stat ast;		// archive attributes at time of init
  bool initialized;
  bool isreg;

public:
  Archive_attrs() : initialized( false ), isreg( false ) {}
  bool init( const int fd )
    {
    if( fstat( fd, &ast ) != 0 ) return false;
    if( S_ISREG( ast.st_mode ) ) isreg = true;
    initialized = true;
    return true;
    }
  bool is_the_archive( const struct stat & st ) const
    { return isreg && st.st_dev == ast.st_dev && st.st_ino == ast.st_ino; }
  bool is_newer( const struct stat & st ) const
    { return initialized && st.st_mtime > ast.st_mtime; }
  bool is_newer( const char * const filename ) const
    {
    if( !initialized ) return false;
    struct stat st;
    return lstat( filename, &st ) != 0 || st.st_mtime > ast.st_mtime;
    }
  };

extern Archive_attrs archive_attrs;


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

const char * const cant_stat = "Can't stat input file";

// defined in create.cc
int parse_cl_arg( const Cl_options & cl_opts, const int i,
                  int (* add_memberp)( const char * const filename,
                      const struct stat *, const int flag, struct FTW * ) );

// defined in create_lz.cc
int encode_lz( const Cl_options & cl_opts, const char * const archive_namep,
               const int outfd );

// defined in create_un.cc
int encode_un( const Cl_options & cl_opts, const char * const archive_namep,
               const int outfd );
