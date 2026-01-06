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

#include <pthread.h>

void xinit_mutex( pthread_mutex_t * const mutex );
void xinit_cond( pthread_cond_t * const cond );
void xdestroy_mutex( pthread_mutex_t * const mutex );
void xdestroy_cond( pthread_cond_t * const cond );
void xlock( pthread_mutex_t * const mutex );
void xunlock( pthread_mutex_t * const mutex );
void xwait( pthread_cond_t * const cond, pthread_mutex_t * const mutex );
void xsignal( pthread_cond_t * const cond );
void xbroadcast( pthread_cond_t * const cond );

// non-pthread_* declarations are in tarlz.h

const char * const conofin_msg = "courier not finished.";
