/*
  Copyright (c) 2023 MariaDB Foundation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  Clevis key management plugin. This communicates with a Tang
  server to reconstruct keys.
*/

#include <mysql/plugin_encryption.h>
#include <mysql/service_encryption.h>
#include <mysql/service_sql.h>
#include <mysql/service_json.h>
#include <mysql/service_my_crypt.h>

#include <jose/jws.h>
#include <stdlib.h>
#include <curl/curl.h>

#define KEY_TABLE "mysql.clevis_keys"
#define SERVER_TABLE "mysql.clevis_servers"

static char *tang_server; /* TODO has large race conditions if dynamic */

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  DYNAMIC_STRING *content= (DYNAMIC_STRING *) userdata;
  dynstr_append_mem(content, ptr, nmemb);
  return nmemb;
}

static char *fetch_jws(DYNAMIC_STRING *qks)
{
  DYNAMIC_STRING url;
  DYNAMIC_STRING content;
  const char *key_start, *key_end;
  int res, comma_pos;

  init_dynamic_string(&url, "http://", 256, 1024);
  dynstr_append(&url, tang_server);
  dynstr_append(&url, "/adv");
  
  init_dynamic_string(&content, NULL, 256, 1024);

  CURL *curl = curl_easy_init();
  if(curl) {
    CURLcode res;
    curl_easy_setopt(curl, CURLOPT_URL, url->str);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
  }
  res= json_locate_key(content.str, content.str + content.length, "payload", key_start, &key_end, &comma_pos);


  if (!jose_jws_ver(NULL, content.str, NULL, NULL, false))
    goto verification; /* failure TODO */
}

static int new_key(MYSQL *mariadb, DYNAMIC_STRING *q)
{
  DYNAMIC_STRING qks;
  MYSQL_RES *res;
  MYSQL_ROW row;
  int result= 1;

  if (init_dynamic_string(&qks, "INSERT IGNORE INTO " SERVER_TABLE " SET key_server = \"", 256, 1024))
    goto ret;
  dynstr_append(&qks, tang_server);
  dynstr_append(&qks, "\" RETURNING jws");
  if (mysql_real_query(mariadb, qks->str, qks->len))
    goto free_qks;

  if ((res= mysql_use_result(mariadb)) == NULL)
    goto free_q;
  row= mysql_fetch_row(res);
  if (row)
  {
    char *cert= row[0];
    if (cert == NULL) // No cert aka new entry
    {
      mysql_free_result(res);
      dynstr_set(&qks, "UPDATE " SERVER_TABLE " SET jws = \"");
      cert= fetch_jws_key(&qks)
      if (!cert)
        goto free_qks;

      new_client_key(q, cert);
    }
    else
      new_client_key(q, cert);

    mysql_free_result(res);
    dynstr_free(qks);
    return 0;
  }

  mysql_free_result(res);
  result= 0;


free_qks:
  dynstr_free(qks);
reterror:
  return result;
}

static unsigned int
get_latest_key_version(unsigned int key_id)
{
  DYNAMIC_STRING q;
  char k_s[16];
  char v_s[16];
  MYSQL_RES *res;
  MYSQL_ROW row;
  unsigned int key_version= ENCRYPTION_KEY_VERSION_INVALID;
  MYSQL *mariadb;

  if ((mariadb= mysql_init(NULL)) == NULL)
    return 1;

  if (init_dynamic_string(&q, "SELECT key_version FROM " KEY_TABLE " WHERE key_id = ", 256, 1024))
    goto ret;

  snprint(k_s, sizeof(k_s), "%d", key_id);
  dynstr_append(&q, k_s);

  if (mysql_real_query(mariadb, q->str, q->len))
    goto sql_error;

  if ((res= mysql_use_result(mariadb)) == NULL)
    goto sql_error;

  row= mysql_fetch_row(res);
  if (row)
  {
    key_version = atoi(row[0]); /* FOUND! */
    goto free_result;
  }
  mysql_free_result(res);

  // New key
  key_version= 1;
  snprint(v_s, sizeof(v_s), "%d", key_version);
  dynstr_append(&q, " AND key_version= ");
  dynstr_append(&q, v_s);
  dynstr_append(&q, " FOR UPDATE");

  if (mysql_real_query(mysql, STRING_WITH_LEN("START TRANSACTION")))
    goto sql_error;

  // SELECT FOR UPDATE
  if (mysql_real_query(mysql, q->str, len))
    goto sql_error; /* TODO deadlock detection */

  dynstr_set(&q, "INSERT INTO " KEY_TABLE " VALUES (");
  dynstr_append(&q, k_s);
  dynstr_append(&q, ",");
  dynstr_append(&q, v_s);
  dynstr_append(&q, ", \"");
  dynstr_append(&q, tang_server);
  dynstr_append(&q, "\", ");
  // KEY
  if (new_key(mariadb, &q))
  {
    if (mysql_real_query(mysql, STRING_WITH_LEN("ROLLBACK")))
      goto sql_error;
  }
  dynstr_append(&q, ")");

  // Save new key
  if (mysql_real_query(mariadb, q->str, q->len))
    goto sql_error;

  if (mysql_real_query(mariadb, STRING_WITH_LEN("COMMIT")))
    goto sql_error;

  goto free_q;

free_result:
  mysql_free_result(res);

  goto free_q;

sql_error:
  if (mysql_error(mariadb)[0])
  {
    my_printf_error(ER_UNKNOWN_ERROR, "clevis: get_latest_key_version %u - SQL error on %s - %d %s",
	 key_version, q->str, mysql_errno(mariadb), mysql_error(mariadb));
  }

free_q:
  dynstr_free(&q);
ret:
  mysql_close(mariadb);

  return key_version;
}

static unsigned int
get_key(unsigned int key_id, unsigned int version,
        unsigned char* dstbuf, unsigned *buflen)
{
  DYNAMIC_STRING q;
  char k_s[16];
  char v_s[16];
  MYSQL_RES *res;
  MYSQL_ROW row;
  unsigned int key_version= ENCRYPTION_KEY_VERSION_INVALID;
  MYSQL *mariadb;
  int result= 1; /* 1 is SQL interface errors, 2 is dynamic_str errors */

  if (*buflen < MY_AES_BLOCK_SIZE)
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;

  if ((mariadb= mysql_init(NULL)) == NULL)
    return 1;

  if (init_dynamic_string(&q, "SELECT key FROM " KEY_TABLE " WHERE key_id = ", 256, 1024))
  {
    result= 2;
    goto exit;
  }

  snprint(k_s, sizeof(k_s), "%d", key_id);
  dynstr_append(&q, k_s);
  dynstr_append(&q, " AND key_version = ");
  snprint(k_s, sizeof(k_s), "%d", version);
  dynstr_append(&q, k_s);

  if (mysql_real_query(mariadb, q->str, q->len))
    goto free_q;

  if ((res= mysql_use_result(mariadb)) == NULL)
    goto free_q;
  row= mysql_fetch_row( res)
  if (row)
  {
    key_version = atoi(row[0]);
    // TODO generate key with server
    result= 0;
  }
  else
    result= ENCRYPTION_KEY_VERSION_INVALID;

  mysql_free_result(res);

free_q:
  dynstr_free(&q);

exit:
  mysql_close(mariadb);

  return result;
}

static int ctx_init(void *ctx, const unsigned char* key, unsigned int klen, const
             unsigned char* iv, unsigned int ivlen, int flags, unsigned int
             key_id, unsigned int key_version)
{
  return my_aes_crypt_init(ctx, MY_AES_GCM, flags, key, klen, iv, ivlen);
}

static unsigned int get_length(unsigned int slen, unsigned int key_id,
                               unsigned int key_version)
{
  return my_aes_get_size(MY_AES_GCM, slen);
}

static int clevis_key_management_plugin_init(void *p)
{
  MYSQL *mariadb;
  int result= 1;

  /* init plugin/test_sql_service/test_sql_service.c */
  if ((mariadb= mysql_init(NULL)) == NULL)
    return 1;
  if (mysql_real_connect_local(mariadb) == NULL)
    return 1;
  if (mysql_real_query(mariadb), STRING_WITH_LEN(
     "CREATE TABLE IF NOT EXISTS " KEY_TABLE " ("
     "  key_id INT UNSIGNED NOT NULL,"
     "  key_version INT UNSIGNED NOT NULL,"
     "  key_server VARBINARY(64) NOT NULL,"
     "  key VARBINARY((16) NOT NULL," /* TODO Are we encoding? */
     "  PRIMARY KEY (key_id, key_version)"
     ") ENGINE=InnoDB"))
    goto exit;
  if (mysql_real_query(mariadb), STRING_WITH_LEN(
     "CREATE TABLE IF NOT EXISTS " SERVER_TABLE " ("
     "  server VARBINARY(64) NOT NULL PRIMARY KEY,"
     "  verify VARBINARY(2048)"
     "  enc VARBINARY(2048)"
     ") ENGINE=InnoDB"))
    goto exit;

  result= 0;

exit:
  mysql_close(mariadb);

  return result;
}

static int clevis_key_management_plugin_deinit(void *p)
{
  return 0;
}


static int ctx_update(void *ctx, const unsigned char *src, unsigned int slen,
  unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_update(ctx, src, slen, dst, dlen);
}


int ctx_finish(void *ctx, unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_finish(ctx, dst, dlen);
}

static  uint ctx_size(unsigned int key_id __attribute__((unused)), unsigned int key_version __attribute__((unused)))
{
  return my_aes_ctx_size(MY_AES_GCM);
}

static MYSQL_SYSVAR_STR(tang_server, tang_server,
                        PLUGIN_VAR_OPCMDARG,
                        "The Tang server for key exchange.",
                        NULL, NULL, "locahost");
static struct st_mysql_sys_var* clevis_sql_vars[]=
{
  MYSQL_SYSVAR(tang_server),
  NULL
};


struct st_mariadb_encryption clevis_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_key_version,
  get_key,
  ctx_size,
  ctx_init,
  ctx_update,
  ctx_finish,
  get_length
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(clevis_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
  &clevis_key_management_plugin,
  "clevis_key_management",
  "Daniel Black",
  "Celvis key management plugin",
  PLUGIN_LICENSE_GPL,
  clevis_key_management_plugin_init,
  clevis_key_management_plugin_deinit,
  0x0001 /* 0.1 */,
  NULL,	/* status variables */
  clevis_sql_vars, /* system variables */
  "0.1",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
