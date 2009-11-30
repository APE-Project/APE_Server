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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <mysql/errmsg.h>

#include "mysac.h"

int mysac_socket_connect(const char *socket_name, int *fd) {
	int ret_code;
	int listen_socket;
	int conf_socket_type = -1;
	struct sockaddr_storage conf_adress;
	int one = 1;
	char *error;
	int already_binded = 0;
	int i;
	char *conf_addr;
	int conf_port;
	char path[1024];
	
	memset(&conf_adress, 0, sizeof(struct sockaddr_storage));

	// -----------------------------------
	//  detect address type 
	// -----------------------------------

	// copy data
	strncpy(path, socket_name, 1024);

	// on cherche le separateur ':'
	for(i=strlen(path)-1; i>0; i--)
		if (path[i]==':')
			break;

	// si on l'à trouvé, on sépare l'indicateur de reseau du port
	if (path[i]==':') {
		path[i] = '\0';
		conf_addr = path;
		conf_port = strtol(&path[i+1], &error, 10);
		if (*error != '\0')
			return MYERR_BAD_PORT;
	}

	// si on ne trouve pas de separateur, ben c'est
	// que c'est une socket unxi
	else
		conf_socket_type = AF_UNIX;

	// AF_UNIX detected create pipe and buils structur
	if ( conf_socket_type == AF_UNIX ) {
		((struct sockaddr_un *)&conf_adress)->sun_family = AF_UNIX;
		strncpy(((struct sockaddr_un *)&conf_adress)->sun_path, path,
		        sizeof(((struct sockaddr_un *)&conf_adress)->sun_path) - 1);

		goto end_of_building_address;
	}

	// AF_INET detected, builds address structur
	ret_code = inet_pton(AF_INET, conf_addr,
	                   &((struct sockaddr_in *)&conf_adress)->sin_addr.s_addr);
	if (ret_code > 0) {
		conf_socket_type = AF_INET;
		((struct sockaddr_in *)&conf_adress)->sin_port = htons(conf_port);
		((struct sockaddr_in *)&conf_adress)->sin_family = AF_INET;
		goto end_of_building_address;
	}

	// AF_INET6 detected, builds address structur
	ret_code = inet_pton(AF_INET6, conf_addr,
	                &((struct sockaddr_in6 *)&conf_adress)->sin6_addr.s6_addr);
	if (ret_code > 0) {
		conf_socket_type = AF_INET6;
		((struct sockaddr_in6 *)&conf_adress)->sin6_port = htons(conf_port);
		((struct sockaddr_in6 *)&conf_adress)->sin6_family = AF_INET6;
		goto end_of_building_address;
	}

	// adress format error
	return MYERR_RESOLV_HOST;

	end_of_building_address:

	// open socket for AF_UNIX
	if (conf_socket_type == AF_UNIX) {
		listen_socket = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listen_socket < 0)
			return MYERR_SYSTEM;
	}

	// open socket for network protocol
	else {
		listen_socket = socket(conf_socket_type, SOCK_STREAM, IPPROTO_TCP);
		if (listen_socket < 0)
			return MYERR_SYSTEM;
	}

	// set non block opt
	ret_code = fcntl(listen_socket, F_SETFL, O_NONBLOCK);
	if (ret_code < 0) {
		close(listen_socket);
		return MYERR_SYSTEM;
	}

	// tcp no delay
	if (conf_socket_type == AF_INET6 || conf_socket_type == AF_INET ) {
		ret_code = setsockopt(listen_socket, IPPROTO_TCP, TCP_NODELAY,
		                      (char *)&one, sizeof(one));
		if (ret_code < 0) {
			close(listen_socket);
			return MYERR_SYSTEM;
		}
	}

	// reuse addr
	if (conf_socket_type == AF_INET6 || conf_socket_type == AF_INET ) {
		ret_code = setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
		                      (char *)&one, sizeof(one));
		if (ret_code < 0) {
			close(listen_socket);
			return MYERR_SYSTEM;
		}
	}

	// bind address
	if (already_binded == 0) {
		switch (conf_socket_type) {
			case AF_INET:
				ret_code = connect(listen_socket,
				                   (struct sockaddr *)&conf_adress,
				                   sizeof(struct sockaddr_in));
				break;

			case AF_INET6:
				ret_code = connect(listen_socket,
				                   (struct sockaddr *)&conf_adress,
				                   sizeof(struct sockaddr_in6));
				break;
		
			case AF_UNIX:
				ret_code = connect(listen_socket,
				                   (struct sockaddr *)&conf_adress,
				                   sizeof(struct sockaddr_un));
				break;
		}
		if (ret_code < 0 && errno != EINPROGRESS){
			close(listen_socket);
			return MYERR_SYSTEM;
		}
	}

	/* return filedescriptor */
	*fd = listen_socket;
	return 0;
}

int mysac_socket_connect_check(int fd) {
	int ret;
	int code;
	size_t len = sizeof(int);

	ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &len);
	if (ret != 0) {
		return MYERR_SYSTEM;
	}
	if (code != 0) {
		return MYERR_CANT_CONNECT;
	}
	return 0;
}

ssize_t mysac_read(int fd, void *buf, size_t count, int *err) {
	ssize_t len;

	len = read(fd, buf, count);

	if (len == 0) {
		*err = MYERR_SERVER_LOST;
		return -1;
	}
	
	if (len == -1) {
		if (errno == EAGAIN)
			*err = MYERR_WANT_READ;
		else
			*err = MYERR_SERVER_LOST;
		return -1;
	}

	*err = 0;
	return len;
}

ssize_t mysac_write(int fd, const void *buf, size_t len, int *err) {
	ssize_t ret;

	ret = write(fd, buf, len);

	if (ret == -1) {
		if (errno == EAGAIN) 
			*err = MYERR_WANT_WRITE;
		else
			*err = MYERR_SERVER_LOST;
		return -1;
	}

	*err = 0;
	return ret;
}

