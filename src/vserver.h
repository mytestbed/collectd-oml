/**
 * collectd - src/vserver.h
 * Copyright (C) 2006  Sebastian Harl
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Sebastian Harl <sh at tokkee.org>
 **/

#if !COLLECTD_VSERVER_H
#define COLLECTD_VSERVER_H 1

#define BUFSIZE 512

#define MODULE_NAME "vserver"
#define PROCDIR "/proc/virtual"

void module_register(void);

#endif /* !COLLECTD_VSERVER_H */

/* vim: set ts=4 sw=4 noexpandtab : */