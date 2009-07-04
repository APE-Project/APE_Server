
#ifndef _APE_MODULE_MYSQL_H
#define _APE_MODULE_MYSQL_H


#include <mysql/mysql.h>
#include "plugins.h"

MYSQL *mysql_instance(acetables *g_ape);
MYSQL *ape_mysql_query(const char *query, acetables *g_ape);
MYSQL *ape_mysql_queryf(acetables *g_ape, const char *buf, ...);
MYSQL_RES *ape_mysql_select(const char *query, acetables *g_ape);
MYSQL_RES *ape_mysql_selectf(acetables *g_ape, char *buf, ...);
MYSQL_ROW ape_mysql_row(const char *query, MYSQL_RES **res, acetables *g_ape);
char *ape_mysql_get(const char *query, acetables *g_ape);
char *ape_mysql_real_escape_string(const char *str, acetables *g_ape);

#endif
