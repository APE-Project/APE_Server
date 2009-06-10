#define _GNU_SOURCE

#include "libape-mysql.h"

#include "plugins.h"
#include "global_plugins.h"

#include <stdio.h>

#define MODULE_NAME "mysql"


int ape_mysql_connect(acetables *g_ape);

static ace_plugin_infos infos_module = {
	"MySQL support",
	"0.01",
	"Anthony Catel",
	"mod_mysql.conf"
};

static void init_module(acetables *g_ape)
{
	MYSQL *mysql = mysql_init(NULL);
	
	add_property(&g_ape->properties, "mysql", mysql, EXTEND_POINTER, EXTEND_ISPRIVATE);
	
	/* init the connection to MySQL */
	if (!ape_mysql_connect(g_ape)) {
		fprintf(stderr, "[Module] [ERR] Connexion SQL : %s\n", mysql_error(mysql));
		exit(0);
	}
}

MYSQL *mysql_instance(acetables *g_ape)
{
	return get_property(g_ape->properties, "mysql")->val;
}


int ape_mysql_connect(acetables *g_ape)
{
	MYSQL *mysql = mysql_instance(g_ape);
	
	if (!mysql_real_connect(mysql, READ_CONF("db_server"), READ_CONF("db_user"), READ_CONF("db_password"), READ_CONF("db_name"), 0, NULL, 0)) { // Faire une function generique
		return 0;
	}
	mysql->reconnect = 1;
	return 1;
}

MYSQL *ape_mysql_query(const char *query, acetables *g_ape)
{
	MYSQL *mysql = mysql_instance(g_ape);

	if (mysql_query(mysql, query)) {
		fprintf(stderr, "[Module] [SQL-WARN] : %s\n", mysql_error(mysql));
		return NULL;
	}
	return mysql;
}

MYSQL *ape_mysql_queryf(acetables *g_ape, const char *buf, ...)
{
	MYSQL *mysql;
	
	char *buff;

	va_list val;
	
	va_start(val, buf);
	vasprintf(&buff, buf, val);
	va_end(val);
	
	mysql = ape_mysql_query(buff, g_ape);
	
	free(buff);
	
	return mysql;
}


MYSQL_RES *ape_mysql_select(const char *query, acetables *g_ape)
{
	// must be free'd
	MYSQL_RES *res;
	MYSQL *mysql = ape_mysql_query(query, g_ape);
	
	if (mysql == NULL) {
		return NULL;
	}
	
	res = mysql_store_result(mysql);
	if (res != NULL && !mysql_num_rows(res)) {
		mysql_free_result(res);
		return NULL;
	}
	return res;
}

MYSQL_ROW ape_mysql_row(const char *query, MYSQL_RES **res, acetables *g_ape)
{
	*res = ape_mysql_select(query, g_ape);
	if (*res == NULL) {
		return NULL;
	}
	
	return mysql_fetch_row(*res);
}

MYSQL_RES *ape_mysql_selectf(acetables *g_ape, char *buf, ...)
{
	MYSQL_RES *res;
	
	char *buff;

	va_list val;
	
	va_start(val, buf);
	vasprintf(&buff, buf, val);
	va_end(val);
	
	res = ape_mysql_select(buff, g_ape);
	
	free(buff);
	
	return res;
}


char *ape_mysql_get(const char *query, acetables *g_ape)
{
	MYSQL_RES *res;
	MYSQL_ROW row;
	char *field;
	
	row = ape_mysql_row(query, &res, g_ape);
	if (row != NULL) {
		field = xstrdup(row[0]);
		mysql_free_result(res);
		
		return field;
	}
	return NULL;
}

static ace_callbacks callbacks = {
	NULL,				/* Called when new user is added */
	NULL,				/* Called when a user is disconnected */
	NULL,				/* Called when new chan is created */
	NULL,				/* Called when a user join a channel */
	NULL				/* Called when a user leave a channel */
};

APE_INIT_PLUGIN(MODULE_NAME, init_module, callbacks)

