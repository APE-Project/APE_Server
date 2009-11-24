/*
 * Copyright (c) 2009 Thierry FOURNIER
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License.
 *
 */

/** @file */ 

#ifndef __MYSAC_H__
#define __MYSAC_H__

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <mysql/errmsg.h>
#include <mysql/mysql.h>

/* def imported from: linux-2.6.24/include/linux/stddef.h */
#define mysac_offset_of(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

/**
 * container_of - cast a member of a structure out to the containing structure
 *
 * def imported from: linux-2.6.24/include/linux/kernel.h 
 *
 * @param ptr    the pointer to the member.
 * @param type   the type of the container struct this is embedded in.
 * @param member the name of the member within the struct.
 *
 */
#define mysac_container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member ) *__mptr = (ptr); \
	(type *)( (char *)__mptr - mysac_offset_of(type,member) );})

/**
 * Simple doubly linked list implementation.
 *
 * def imported from: linux-2.6.24/include/linux/list.h
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */
struct mysac_list_head {
	struct mysac_list_head *next, *prev;
};

/**
 * list_entry - get the struct for this entry
 *
 * def imported from: linux-2.6.24/include/linux/list.h
 *
 * @param ptr: the &struct list_head pointer.
 * @param type:   the type of the struct this is embedded in.
 * @param member: the name of the list_struct within the struct.
 */
#define mysac_list_entry(ptr, type, member) \
	mysac_container_of(ptr, type, member)

/**
 * list_first_entry - get the first element from a list
 *
 * def imported from: linux-2.6.24/include/linux/list.h
 * @param ptr    the list head to take the element from.
 * @param type   the type of the struct this is embedded in.
 * @param member the name of the list_struct within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define mysac_list_first_entry(ptr, type, member) \
	mysac_list_entry((ptr)->next, type, member)

#define mysac_list_next_entry(ptr, type, member) \
	mysac_list_first_entry(ptr, type, member);

enum my_query_st {
	MYSAC_START,

	MYSAC_CONN_CHECK,
	MYSAC_READ_GREATINGS,
	MYSAC_SEND_AUTH_1,
	MYSAC_RECV_AUTH_1,
	MYSAC_SEND_AUTH_2,

	MYSAC_SEND_QUERY,
	MYSAC_RECV_QUERY_COLNUM,
	MYSAC_RECV_QUERY_COLDESC,
	MYSAC_RECV_QUERY_EOF1,
	MYSAC_RECV_QUERY_DATA,

	MYSAC_SEND_INIT_DB,
	MYSAC_RECV_INIT_DB,

	MYSAC_SEND_STMT_QUERY,
	MYSAC_RECV_STMT_QUERY,

	MYSAC_SEND_STMT_EXECUTE,
	MYSAC_RECV_STMT_EXECUTE,

	MYSAC_READ_NUM,
	MYSAC_READ_HEADER,
	MYSAC_READ_LINE
};

#define MYSAC_COL_MAX_LEN 50
#define MYSAC_COL_MAX_NUN 100

/* errors */
enum {
	MYERR_PROTOCOL_ERROR = 1,
	MYERR_BUFFER_OVERSIZE,
	MYERR_PACKET_CORRUPT,
	MYERR_WANT_READ,
	MYERR_WANT_WRITE,
	MYERR_UNKNOWN_ERROR,
	MYERR_MYSQL_ERROR,
	MYERR_SERVER_LOST,
	MYERR_BAD_PORT,
	MYERR_BAD_STATE,
	MYERR_RESOLV_HOST,
	MYERR_SYSTEM,
	MYERR_CANT_CONNECT,
	MYERR_BUFFER_TOO_SMALL,
	MYERR_UNEXPECT_R_STATE,
	MYERR_STRFIELD_CORRUPT,
	MYERR_BINFIELD_CORRUPT,
	MYERR_BAD_LCB,
	MYERR_LEN_OVER_BUFFER,
	MYERR_CONVLONG,
	MYERR_CONVLONGLONG,
	MYERR_CONVFLOAT,
	MYERR_CONVDOUBLE,
	MYERR_CONVTIME,
	MYERR_CONVTIMESTAMP,
	MYERR_CONVDATE
};

extern const char *mysac_type[];
extern const char *mysac_errors[];

/**
 * This is union containing all c type matching mysql types
 */
typedef union {
	signed char stiny;          /* MYSQL_TYPE_TINY      TINYINT */
	unsigned char utiny;        /* MYSQL_TYPE_TINY      TINYINT */
	unsigned char mbool;        /* MYSQL_TYPE_TINY      TINYINT */
	short ssmall;               /* MYSQL_TYPE_SHORT     SMALLINT */
	unsigned short small;       /* MYSQL_TYPE_SHORT     SMALLINT */
	int sint;                   /* MYSQL_TYPE_LONG      INT */
	unsigned int uint;          /* MYSQL_TYPE_LONG      INT */
	long long sbigint;          /* MYSQL_TYPE_LONGLONG  BIGINT */
	unsigned long long ubigint; /* MYSQL_TYPE_LONGLONG  BIGINT */
	float mfloat;               /* MYSQL_TYPE_FLOAT     FLOAT */
	double mdouble;             /* MYSQL_TYPE_DOUBLE    DOUBLE */
	struct timeval tv;          /* MYSQL_TYPE_TIME      TIME */
	struct tm *tm;              /* MYSQL_TYPE_DATE      DATE
	                               MYSQL_TYPE_DATETIME  DATETIME
	                               MYSQL_TYPE_TIMESTAMP TIMESTAMP */
	char* string;               /* MYSQL_TYPE_STRING    TEXT,CHAR,VARCHAR */
	char* blob;                 /* MYSQL_TYPE_BLOB      BLOB,BINARY,VARBINARY */
	void *ptr;                  /* generic pointer */
} MYSAC_ROW;

/**
 * This is chained element. contain pointer to each elements of one row
 */
typedef struct {
	struct mysac_list_head link;
	unsigned long *lengths;
	MYSAC_ROW *data;
} MYSAC_ROWS;

/**
 * This contain the complete result of one request
 */
typedef struct {
	char *buffer;
	int buffer_len;
	int nb_cols;
	int nb_lines;
	int nb_time;
	MYSQL_FIELD *cols;
	struct mysac_list_head data;
	MYSAC_ROWS *cr;
} MYSAC_RES;

/**
 * This contain the necessary for one mysql connection
 */
typedef struct mysac {
	int len;
	char *read;
	int read_len;
	char *send;
	int readst;

	unsigned int packet_length;
	unsigned int packet_number;

	/* mysac */
	char free_it;
	int fd;
	int (*call_it)(/* MYSAC * */ struct mysac *);

	/*defconnect */
	unsigned int protocol;
	char *version;
	unsigned int threadid;
	char salt[SCRAMBLE_LENGTH + 1]; 
	unsigned int options;
	unsigned int charset;
	unsigned int status;
	unsigned long affected_rows;
	unsigned int warnings;
	unsigned int errorcode;
	unsigned long insert_id;
	char *mysql_code;
	char *mysql_error;
	char eof;

	/* user */
	const char *addr;
	const char *login;
	const char *password;
	const char *database;
	const char *query;
	unsigned int flags;

	/* query */
	enum my_query_st qst;
	int read_id;
	MYSAC_RES *res;

	/* the buffer */
	unsigned int bufsize;
	char *buf;
} MYSAC;

/**
 * Allocates and initializes a MYSQL object.
 * If mysql is a NULL pointer, the function allocates, initializes, and
 * returns a new object. Otherwise, the object is initialized and the address
 * of the object is returned. If mysql_init() allocates a new object, it is
 * freed when mysql_close() is called to close the connection.
 *
 * @param buffsize is the size of the buffer
 *
 * @return An initialized MYSAC* handle. NULL if there was insufficient memory
 *         to allocate a new object.
 */
MYSAC *mysac_new(int buffsize);

/**
 * Initializes a MYSQL object.
 * If mysql is a NULL pointer, the function allocates, initializes, and
 * returns a new object. Otherwise, the object is initialized and the address
 * of the object is returned. If mysql_init() allocates a new object, it is
 * freed when mysql_close() is called to close the connection.
 *
 * @param mysac Should be the address of an existing MYSAC structure.
 * @param buffer is ptr on the pre-allocated buffer
 * @param buffsize is the size of the buffer
 */
void mysac_init(MYSAC *mysac, char *buffer, unsigned int buffsize);

/**
 * mysac_connect() attempts to establish a connection to a MySQL database engine
 * running on host. mysql_real_connect() must complete successfully before you
 * can execute any other API functions that require a valid MYSQL connection
 * handle structure.
 *
 * @param mysac The first parameter should be the address of an existing MYSQL
 *        structure. Before calling mysql_real_connect() you must call
 *        mysql_init() to initialize the MYSQL structure. You can change a lot
 *        of connect options with the mysql_options() call.
 *
 * @param my_addr like "<ipv4>:<port>" "<ipv6>:<port>", "socket_unix_file" or
 *        NULL. If NULL, bind is set to socket 0
 *
 * @param user The user parameter contains the user's MySQL login ID. If user
 *        is NULL or the empty string "", the current user is assumed.
 *
 * @param passwd The passwd parameter contains the password for user. If passwd
 *        is NULL, only entries in the user table for the user that have a blank
 *        (empty) password field are checked for a match.
 *
 * @param db is the database name. If db is not NULL, the connection sets the
 *        default database to this value.
 *
 * @param client_flag The value of client_flag is usually 0, but can be set to a
 *        combination of the following flags to enable certain features:
 *
 *        Flag Name                Flag Description
 *        CLIENT_COMPRESS          Use compression protocol.
 *        CLIENT_FOUND_ROWS        Return the number of found (matched) rows,
 *                                 not the number of changed rows.
 *        CLIENT_IGNORE_SPACE      Allow spaces after function names. Makes all
 *                                 functions names reserved words.
 */
void mysac_setup(MYSAC *mysac, const char *my_addr, const char *user,
                 const char *passwd, const char *db,
                 unsigned long client_flag);

/**
 * Run network connexion and mysql authentication
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 *
 * @return
 * 	MYERR_WANT_READ        : want read socket
 * 	MYERR_WANT_WRITE       : want write socket
 *    CR_CONN_HOST_ERROR     : Failed to connect to the MySQL server.
 *    CR_CONNECTION_ERROR    : Failed to connect to the local MySQL server.
 *    CR_IPSOCK_ERROR        : Failed to create an IP socket.
 *    CR_OUT_OF_MEMORY       : Out of memory.
 *    CR_SOCKET_CREATE_ERROR : Failed to create a Unix socket.
 *    CR_UNKNOWN_HOST        : Failed to find the IP address for the hostname.
 *    CR_VERSION_ERROR       : A protocol mismatch resulted from attempting to
 *                             connect to a server with a client library that
 *                             uses a different protocol version.
 *    CR_SERVER_LOST         : If connect_timeout > 0 and it took longer than
 *                             connect_timeout seconds to connect to the server
 *                             or if the server died while executing the
 *                             init-command.
 */
int mysac_connect(MYSAC *mysac);

/**
 * Closes a previously opened connection. mysql_close() also deallocates the
 * connection handle pointed to by mysql if the handle was allocated
 * automatically by mysql_init() or mysql_connect().
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 */
static inline
void mysac_close(MYSAC *mysac) {
	if (mysac->free_it == 1)
		free(mysac);
}

/**
 * This function return the mysql filedescriptor used for connection
 * to the mysql server
 *
 * @param mysac Should be the address of an existing MYSAC structure.
 *
 * @return mysql filedescriptor
 */
static inline
int mysac_get_fd(MYSAC *mysac) {
	return mysac->fd;
}

/**
 * this function call the io function associated with the current
 * command. (mysac_send_database, mysac_send_query and mysac_connect)
 *
 * @param mysac Should be the address of an existing MYSAC structure.
 *
 * @return 0 is ok, or all errors associated with functions
 *         mysac_send_database, mysac_send_query and mysac_connect or
 *    MYERR_BAD_STATE : the function does nothing to do (is an error)
 */
static inline
int mysac_io(MYSAC *mysac) {
	if (mysac == NULL || mysac->call_it == NULL)
		return MYERR_BAD_STATE;
	return mysac->call_it(mysac);
}

/**
 * Build use database message
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 * @param database is the database name
 */
int mysac_set_database(MYSAC *mysac, const char *database);

/**
 * This send use database command
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 *
 * @return
 *  0 => ok
 *  MYSAC_WANT_READ
 *  MYSAC_WANT_WRITE
 *  ...
 */
int mysac_send_database(MYSAC *mysac);

/**
 * Prepare statement
 *
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param fmt is the output format with the printf style
 *
 * @return 0: ok, -1 nok
 */
int mysac_set_stmt_prepare(MYSAC *mysac, const char *fmt, ...);

/**
 * Send sql query command
 * 
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param stmt_id is pointer for storing the statement id
 *
 * @return
 *  0 => ok
 *  MYSAC_WANT_READ
 *  MYSAC_WANT_WRITE
 *  ...
 */
int mysac_send_stmt_prepare(MYSAC *mysac, unsigned long *stmt_id);

/**
 * Initialize MYSAC_RES structur
 * This function can not allocate memory, just use your buffer.
 *
 * @param buffer this buffer must contain all the sql response.
 *        this size is:
 *        sizeof(MYSAC_RES) +
 *        ( sizeof(MYSQL_FIELD) * nb_field ) + 
 *        ( different fields names )
 *
 *        and for each row:
 *        sizeof(MYSAC_ROWS) +
 *        ( sizeof(MYSAC_ROW) * nb_field ) +
 *        ( sizeof(unsigned long) * nb_field ) +
 *        ( sizeof(struct tm) for differents date fields of the request ) +
 *        ( differents strings returned by the request ) +
 *
 * @param len is the len of the buffer
 *
 * @return MYSAC_RES. this function cannot be fail
 */
static inline
MYSAC_RES *mysac_init_res(char *buffer, int len) {
	MYSAC_RES *res;

	/* check minimu length */
	if (len < sizeof(MYSAC_RES))
		return NULL;

	res = (MYSAC_RES *)buffer;
	res->nb_cols = 0;
	res->nb_lines = 0;
	res->buffer = buffer + sizeof(MYSAC_RES);
	res->buffer_len = len - sizeof(MYSAC_RES);

	return res;
}

/**
 * Execute statement
 *
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param res Should be the address of an existing MYSAC_RES structur.
 * @param stmt_id the statement id
 *
 * @return 0: ok, -1 nok
 */
int mysac_set_stmt_execute(MYSAC *mysac, MYSAC_RES *res, unsigned long stmt_id);

/**
 * Initialize query
 *
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param res Should be the address of an existing MYSAC_RES structur.
 * @param fmt is the output format with the printf style
 *
 * @return 0: ok, -1 nok
 */
int mysac_set_query(MYSAC *mysac, MYSAC_RES *res, const char *fmt, ...);

/**
 * Initialize query
 *
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param res Should be the address of an existing MYSAC_RES structur.
 * @param fmt is the output format with the printf style
 * @param ap is the argument list on format vprintf
 *
 * @return 0: ok, -1 nok
 */
int mysac_v_set_query(MYSAC *mysac, MYSAC_RES *res, const char *fmt, va_list ap);

/**
 * Initialize query
 *
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param res Should be the address of an existing MYSAC_RES structur.
 * @param query is a string (terminated by \0) containing the query
 *
 * @return 0: ok, -1 nok
 */
int mysac_s_set_query(MYSAC *mysac, MYSAC_RES *res, const char *query);

/**
 * Initialize query
 *
 * @param mysac Should be the address of an existing MYSAC structur.
 * @param res Should be the address of an existing MYSAC_RES structur.
 * @param query is a string containing the query
 * @param len is the len of the query
 *
 * @return 0: ok, -1 nok
 */
int mysac_b_set_query(MYSAC *mysac, MYSAC_RES *res, const char *query, int len);

/**
 * This function return the mysql response pointer 
 *
 * @param mysac Should be the address of an existing MYSAC structure.
 *
 * @return mysql response pointer
 */
static inline
MYSAC_RES *mysac_get_res(MYSAC *mysac) {
	return mysac->res;
}

/**
 * Send sql query command
 * 
 * @param mysac Should be the address of an existing MYSAC structur.
 *
 * @return
 *  0 => ok
 *  MYSAC_WANT_READ
 *  MYSAC_WANT_WRITE
 *  ...
 */
int mysac_send_query(MYSAC *mysac);

/**
 * send stmt execute command
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 *
 * @return 
 */
static inline
int mysac_send_stmt_execute(MYSAC *mysac) {
	return mysac_send_query(mysac);
}

/**
 * After executing a statement with mysql_query() returns the number of rows
 * changed (for UPDATE), deleted (for DELETE), orinserted (for INSERT). For
 * SELECT statements, mysql_affected_rows() works like mysql_num_rows().
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 *
 * @return An integer greater than zero indicates the number of rows affected
 *         or retrieved. Zero indicates that no records were updated for an
 *         UPDATE statement, no rows matched the WHERE clause in the query or
 *         that no query has yet been executed. -1 indicates that the query
 *         returned an error or that, for a SELECT query, mysql_affected_rows()
 *         was called prior to calling mysql_store_result(). Because
 *         mysql_affected_rows() returns an unsigned value, you can check for -1
 *         by comparing the return value to (my_ulonglong)-1 (or to
 *         (my_ulonglong)~0, which is equivalent).
 */
int mysac_affected_rows(MYSAC *mysac);

/**
 * Returns the number of columns for the most recent query on the connection.
 *
 * @param res Should be the address of an existing MYSAC_RES structure.
 *
 * @return number of columns
 */
static inline
unsigned int mysac_field_count(MYSAC_RES *res) {
	return res->nb_cols;
}

/**
 * Returns the number of rows in the result set.
 * 
 * mysql_num_rows() is intended for use with statements that return a result
 * set, such as SELECT. For statements such as INSERT, UPDATE, or DELETE, the
 * number of affected rows can be obtained with mysql_affected_rows().
 *
 * @param res Should be the address of an existing MYSAC_RES structure.
 *
 * @return The number of rows in the result set. 
 */
static inline
unsigned long mysac_num_rows(MYSAC_RES *res) {
	return res->nb_lines;
}

/**
 * Retrieves the next row of a result set. mysql_fetch_row() returns NULL when
 * there are no more rows to retrieve or if an error occurred.
 * 
 * The number of values in the row is given by mysql_num_fields(result).
 *
 * The lengths of the field values in the row may be obtained by calling
 * mysql_fetch_lengths(). Empty fields and fields containing NULL both have
 * length 0; you can distinguish these by checking the pointer for the field
 * value. If the pointer is NULL, the field is NULL; otherwise, the field is
 * empty.
 *
 * @param res Should be the address of an existing MYSAC_RES structure.
 * 
 * @return A MYSAC_ROW structure for the next row. NULL if there are no more
 * rows to retrieve or if an error occurred. 
 */
static inline
MYSAC_ROW *mysac_fetch_row(MYSAC_RES *res) {
	if (res->cr == NULL)
		res->cr = mysac_list_first_entry(&res->data, MYSAC_ROWS, link);
	else
		res->cr = mysac_list_next_entry(&res->cr->link, MYSAC_ROWS, link);
	if (&res->data == &res->cr->link) {
		res->cr = NULL;
		return NULL;
	}
	return res->cr->data;
}

/**
 * Set pointer on the first row, you can exec mysac_fetch_row, return it the
 * first row;
 */
static inline
void mysac_first_row(MYSAC_RES *res) {
	res->cr = NULL;
}

/**
 * Get current row, dont touch row ptr
 */
static inline
MYSAC_ROW *mysac_cur_row(MYSAC_RES *res) {
	return res->cr->data;
}

/**
 * Returns the value generated for an AUTO_INCREMENT column by the previous
 * INSERT or UPDATE statement. Use this function after you have performed an
 * INSERT statement into a table that contains an AUTO_INCREMENT field
 *
 * http://dev.mysql.com/doc/refman/5.0/en/mysql-insert-id.html
 *
 * @param m Should be the address of an existing MYSQL structure.
 *
 * @return the value generated for an AUTO_INCREMENT column
 */
static inline
unsigned long mysac_insert_id(MYSAC *m) {
	return m->insert_id;
}



#if 0
mysql_fetch_fields() /*Returns an array of all field structures*/
mysql_fetch_field() /*Returns the type of the next table field*/
mysql_fetch_lengths() /*Returns the lengths of all columns in the current row*/
#endif

/**
 * Changes the user and causes the database specified by db to become the
 * default (current) database on the connection specified by mysql. In
 * subsequent queries, this database is the default for table references that
 * do not include an explicit database specifier.
 *
 * mysql_change_user() fails if the connected user cannot be authenticated or
 * doesn't have permission to use the database. In this case, the user and
 * database are not changed
 *
 * This command resets the state as if one had done a new connect. It always
 * performs a ROLLBACK of any active transactions, closes and drops all
 * temporary tables, and unlocks all locked tables. Session system variables
 * are reset to the values of the corresponding global system variables.
 * Prepared statements are released and HANDLER variables are closed. Locks
 * acquired with GET_LOCK() are released. These effects occur even if the user
 * didn't change.
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 *
 * @param user The user parameter contains the user's MySQL login ID. If user
 *        is NULL or the empty string "", the current user is assumed.
 *
 * @param passwd The passwd parameter contains the password for user. If passwd
 *        is NULL, only entries in the user table for the user that have a blank
 *        (empty) password field are checked for a match.
 *
 * @param db The db parameter may be set to NULL if you don't want to have a
 *        default database.
 *
 * @return 
 *    CR_COMMANDS_OUT_OF_SYNC  : Commands were executed in an improper order.
 *    CR_SERVER_GONE_ERROR     : The MySQL server has gone away.
 *    CR_SERVER_LOST           : The connection to the server was lost during
 *                               the query.
 *    CR_UNKNOWN_ERROR         : An unknown error occurred.
 *    ER_UNKNOWN_COM_ERROR     : The MySQL server doesn't implement this
 *                               command (probably an old server).
 *    ER_ACCESS_DENIED_ERROR   : The user or password was wrong.
 *    ER_BAD_DB_ERROR          : The database didn't exist.
 *    ER_DBACCESS_DENIED_ERROR : The user did not have access rights to the
 *                               database.
 *    ER_WRONG_DB_NAME         : The database name was too long.
 */
int mysac_change_user(MYSAC *mysac, const char *user, const char *passwd,
                      const char *db);

/**
 * Returns the default character set name for the current connection.
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 * 
 * @return The default character set name 
 */
//const char *mysac_character_set_name(MYSAC *mysac);

/**
 * For the connection specified by mysql, mysql_errno() returns the error code
 * for the most recently invoked API function that can succeed or fail. A return
 * value of zero means that no error occurred. Client error message numbers are
 * listed in the MySQL errmsg.h header file. Server error message numbers are
 * listed in mysqld_error.h. Errors also are listed at Appendix B, Errors, Error
 * Codes, and Common Problems. 
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 */
static inline
unsigned int mysac_errno(MYSAC *mysac) {
	return mysac->errorcode;
}

/**
 * For the connection specified by mysql, mysql_error() returns a null-
 * terminated string containing the error message for the most recently invoked
 * API function that failed. If a function didn't fail, the return value of
 * mysql_error() may be the previous error or an empty string to indicate no
 * error.
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 */
static inline 
const char *mysac_error(MYSAC *mysac) {
	return mysac_errors[mysac->errorcode];
}

/**
 * For the connection specified by mysql, mysql_error() returns a null-
 * terminated string containing the error message for the most recently invoked
 * API function that failed. If a function didn't fail, the return value of
 * mysql_error() may be the previous error or an empty string to indicate no
 * error.
 *
 * @param mysac Should be the address of an existing MYSQL structure.
 */
static inline 
const char *mysac_advance_error(MYSAC *mysac) {
	if (mysac->errorcode == MYERR_MYSQL_ERROR)
		return mysac->mysql_error;
	else if (mysac->errorcode == MYERR_SYSTEM)
		return strerror(errno);
	else
		return mysac_errors[mysac->errorcode];
}

/*
1607 ulong STDCALL
1608 mysac_escape_string(MYSQL *mysql, char *to,const char *from,
1609           ulong length)
1610 {
1611   if (mysql->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES)
1612     return escape_quotes_for_mysql(mysql->charset, to, 0, from, length);
1613   return escape_string_for_mysql(mysql->charset, to, 0, from, length);
1614 }
*/

#if 0
mysql_affected_rows() /*Returns the number of rows changed/deleted/inserted by the last UPDATE, DELETE, or INSERT query*/
mysql_autocommit() /*Toggles autocommit mode on/off*/
mysql_commit() /*Commits the transaction*/
mysql_create_db() /*Creates a database (this function is deprecated; use the SQL statement CREATE DATABASE instead)*/
mysql_data_seek() /*Seeks to an arbitrary row number in a query result set*/
mysql_debug() /*Does a DBUG_PUSH with the given string*/
mysql_drop_db() /*Drops a database (this function is deprecated; use the SQL statement DROP DATABASE instead)*/
mysql_dump_debug_info() /*Makes the server write debug information to the log*/
mysql_escape_string() /*Escapes special characters in a string for use in an SQL statement*/
mysql_fetch_field_direct() /*Returns the type of a table field, given a field number*/
mysql_field_seek() /*Puts the column cursor on a specified column*/
mysql_field_tell() /*Returns the position of the field cursor used for the last mysql_fetch_field()*/
mysql_free_result() /*Frees memory used by a result set*/
mysql_get_character_set_info() /*Return information about default character set*/
mysql_get_client_info() /*Returns client version information as a string*/
mysql_get_client_version() /*Returns client version information as an integer*/
mysql_get_host_info() /*Returns a string describing the connection*/
mysql_get_proto_info() /*Returns the protocol version used by the connection*/
mysql_get_server_info() /*Returns the server version number*/
mysql_get_server_version() /*Returns version number of server as an integer*/
mysql_get_ssl_cipher() /*Return current SSL cipher*/
mysql_hex_string() /*Encode string in hexadecimal format*/
mysql_info() /*Returns information about the most recently executed query*/
mysql_init() /*Gets or initializes a MYSQL structure*/
mysql_kill() /*Kills a given thread*/
mysql_library_end() /*Finalize the MySQL C API library*/
mysql_library_init() /*Initialize the MySQL C API library*/
mysql_list_dbs() /*Returns database names matching a simple regular expression*/
mysql_list_fields() /*Returns field names matching a simple regular expression*/
mysql_list_processes() /*Returns a list of the current server threads*/
mysql_list_tables() /*Returns table names matching a simple regular expression*/
mysql_more_results() /*Checks whether any more results exist*/
mysql_next_result() /*Returns/initiates the next result in multiple-statement executions*/
mysql_num_fields() /*Returns the number of columns in a result set*/
mysql_num_rows() /*Returns the number of rows in a result set*/
mysql_options() /*Sets connect options for mysql_real_connect()*/
mysql_ping() /*Checks whether the connection to the server is working, reconnecting as necessary*/
mysql_query() /*Executes an SQL query specified as a null-terminated string*/
mysql_real_connect() /*Connects to a MySQL server*/
mysql_real_escape_string() /*Escapes special characters in a string for use in an SQL statement, taking into account the current character set of the connection*/
mysql_real_query() /*Executes an SQL query specified as a counted string*/
mysql_refresh() /*Flush or reset tables and caches*/
mysql_reload() /*Tells the server to reload the grant tables*/
mysql_rollback() /*Rolls back the transaction*/
mysql_row_seek() /*Seeks to a row offset in a result set, using value returned from mysql_row_tell()*/
mysql_row_tell() /*Returns the row cursor position*/
mysql_select_db() /*Selects a database*/
mysql_server_end() /*Finalize the MySQL C API library*/
mysql_server_init() /*Initialize the MySQL C API library*/
mysql_set_character_set() /*Set default character set for current connection*/
mysql_set_local_infile_default() /*Set the LOAD DATA LOCAL INFILE handler callbacks to their default values*/
mysql_set_local_infile_handler() /*Install application-specific LOAD DATA LOCAL INFILE handler callbacks*/
mysql_set_server_option() /*Sets an option for the connection (like multi-statements)*/
mysql_sqlstate() /*Returns the SQLSTATE error code for the last error*/
mysql_shutdown() /*Shuts down the database server*/
mysql_ssl_set() /*Prepare to establish SSL connection to server*/
mysql_stat() /*Returns the server status as a string*/
mysql_store_result() /*Retrieves a complete result set to the client*/
mysql_thread_id() /*Returns the current thread ID*/
mysql_use_result() /*Initiates a row-by-row result set retrieval*/
mysql_warning_count() /*Returns the warning count for the previous SQL statement*/
#endif


#endif
