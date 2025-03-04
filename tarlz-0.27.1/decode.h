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
