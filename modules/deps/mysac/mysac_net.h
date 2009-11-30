/*
 * Copyright (c) 2009 Thierry FOURNIER
 *
 * This file is part of MySAC.
 *
 * MySAC is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License
 *
 * MySAC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MySAC.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __MYSAC_NET_H__
#define __MYSAC_NET_H__

int mysac_socket_connect(const char *socket_name, int *fd);
int mysac_socket_connect_check(int fd);
ssize_t mysac_read(int fd, void *buf, size_t count, int *err);
ssize_t mysac_write(int fd, const void *buf, size_t len, int *err);

#endif
