# omp-mySQL — Natives reference & migration map

omp-mySQL uses a **modern, industry-standard API** (JDBC / Python DB-API /
Go `database/sql` vocabulary: *Connection*, *PreparedStatement*, *ResultSet*;
verbs *connect / execute / prepare / set / get*). It does **not** reuse
SA-MP-MySQL's native names.

This table lists every native and the **old SA-MP-MySQL (R41-4)** name it
replaces, so you can migrate an existing gamemode.

You still `#include <omp-mysql>`. Tags: `MySQL:` (connection), `MySQLConfig:`
(options), `PreparedStatement:`, `ResultSet:`.

> **Two breaking conventions vs. the old plugin:**
> 1. **Prepared-statement parameters are 1-based** (JDBC-style): the first `?`
>    is index **1**, not 0.
> 2. **Result-set values** live behind the `mysql_rs_*` family (a *ResultSet*),
>    replacing `cache_*`.

---

## Connection

| omp-mySQL | replaces (SA-MP-MySQL) | notes |
|---|---|---|
| `mysql_connect(host, user, pass, db, MySQLConfig:cfg)` | `mysql_connect` | TLS mandatory; user should be `caching_sha2_password`. |
| `mysql_connect_config(file = "mysql.ini")` | `mysql_connect_file` | supports `${VAR}` env-var secrets. |
| `mysql_close(MySQL:h)` | `mysql_close` | |
| `mysql_pending_count(MySQL:h)` | `mysql_unprocessed_queries` | queued/in-flight async ops. |
| `mysql_config_create()` → `MySQLConfig:` | `mysql_init_options` | |
| `mysql_config_set(MySQLConfig:cfg, E_MYSQL_OPTION:t, value)` | `mysql_set_option` | |
| `mysql_global_set(E_MYSQL_GLOBAL_OPTION:t, value)` | `mysql_global_options` | |
| `mysql_set_limit(MySQL:h, E_MYSQL_LIMIT:t, value)` | *(new)* | query guards (rate/length/pending). |

## Errors, status, charset, TLS

| omp-mySQL | replaces | notes |
|---|---|---|
| `mysql_errno(MySQL:h)` | `mysql_errno` | |
| `mysql_error(dst[], len, MySQL:h)` | `mysql_error` | |
| `mysql_server_status(dst[], len, MySQL:h)` | `mysql_stat` | |
| `mysql_set_charset(charset[], MySQL:h)` | `mysql_set_charset` | default `utf8mb4`. |
| `mysql_get_charset(dst[], len, MySQL:h)` | `mysql_get_charset` | |
| `mysql_get_tls_cipher(dst[], len, MySQL:h)` | `mysql_get_ssl_cipher` | |
| `mysql_is_tls_enabled(MySQL:h)` → `bool:` | `mysql_is_ssl_enabled` | |

## Escaping & formatting

| omp-mySQL | replaces | notes |
|---|---|---|
| `mysql_escape(src[], dst[], len, MySQL:h)` | `mysql_escape_string` | prefer prepared statements. |
| `mysql_format(MySQL:h, out[], len, fmt[], ...)` | `mysql_format` | `%e` escape, `%q` quote. |

## Queries

`execute` is the standard verb. The async form is the common path.

| omp-mySQL | replaces | notes |
|---|---|---|
| `mysql_execute(MySQL:h, query[], cb[]="", fmt[]="", ...)` | `mysql_tquery` | async + callback. |
| `mysql_execute_for(MySQL:h, query[], cb[]="", fmt[]="", ...)` | `mysql_pquery` | async, bound to a player id. |
| `mysql_execute_sync(MySQL:h, query[], bool:use_cache=true)` → `ResultSet:` | `mysql_query` | synchronous. |
| `mysql_execute_file(MySQL:h, path[], cb[]="", fmt[]="", ...)` | `mysql_tquery_file` | async from file. |
| `mysql_execute_file_sync(MySQL:h, path[], bool:use_cache=true)` | `mysql_query_file` | sync from file. |

## Prepared statements (recommended — injection-safe, **1-based** params)

| omp-mySQL | replaces | notes |
|---|---|---|
| `mysql_prepare(MySQL:h, query[])` → `PreparedStatement:` | *(new)* | `?` placeholders. |
| `mysql_stmt_set_int(PreparedStatement:s, idx, value)` | *(new)* | idx is 1-based. |
| `mysql_stmt_set_float(PreparedStatement:s, idx, Float:value)` | *(new)* | |
| `mysql_stmt_set_string(PreparedStatement:s, idx, value[])` | *(new)* | |
| `mysql_stmt_set_null(PreparedStatement:s, idx)` | *(new)* | |
| `mysql_stmt_execute(PreparedStatement:s, cb[]="", fmt[]="", ...)` | *(new)* | async; result is the active ResultSet in the callback. |
| `mysql_stmt_close(PreparedStatement:s)` | *(new)* | |

## Password hashing

| omp-mySQL | replaces | notes |
|---|---|---|
| `mysql_hash(password[], cb[], fmt[]="", E_MYSQL_HASH_ALGO:algo=HASH_ARGON2ID, ...)` | *(new)* | async Argon2id. |
| `mysql_hash_sync(password[], dst[], len, algo=HASH_ARGON2ID)` | *(new)* | |
| `mysql_verify(password[], hash[], cb[], fmt[]="", ...)` | *(new)* | async, constant-time. |
| `mysql_verify_sync(password[], hash[])` → `bool:` | *(new)* | |

## VECTOR (MySQL 9.x vector similarity search)

| omp-mySQL | replaces | notes |
|---|---|---|
| `mysql_vector_to_string(Float:vals[], count, dst[], len)` | *(new)* | |
| `mysql_string_to_vector(src[], Float:dst[], len)` | *(new)* | |
| `mysql_vector_dim(src[])` | *(new)* | |

## ResultSet — replaces the whole `cache_*` family

Read inside a query callback (the result is the active ResultSet), or on a
`ResultSet:` handle you retained.

| omp-mySQL | replaces |
|---|---|
| `mysql_rs_row_count(&dst)` | `cache_get_row_count` |
| `mysql_rs_field_count(&dst)` | `cache_get_field_count` |
| `mysql_rs_set_count(&dst)` | `cache_get_result_count` |
| `mysql_rs_field_name(idx, dst[], len)` | `cache_get_field_name` |
| `mysql_rs_field_type(idx)` → `E_MYSQL_FIELD_TYPE:` | `cache_get_field_type` |
| `mysql_rs_select_set(idx)` | `cache_set_result` |
| `mysql_rs_get_string(row, col, dst[], len)` | `cache_get_value_index` |
| `mysql_rs_get_int(row, col, &dst)` | `cache_get_value_index_int` |
| `mysql_rs_get_float(row, col, &Float:dst)` | `cache_get_value_index_float` |
| `mysql_rs_get_bool(row, col, &bool:dst)` | `cache_get_value_index_bool` |
| `mysql_rs_is_null(row, col, &bool:dst)` | `cache_is_value_index_null` |
| `mysql_rs_get_string_by(row, col[], dst[], len)` | `cache_get_value_name` |
| `mysql_rs_get_int_by(row, col[], &dst)` | `cache_get_value_name_int` |
| `mysql_rs_get_float_by(row, col[], &Float:dst)` | `cache_get_value_name_float` |
| `mysql_rs_get_bool_by(row, col[], &bool:dst)` | `cache_get_value_name_bool` |
| `mysql_rs_is_null_by(row, col[], &bool:dst)` | `cache_is_value_name_null` |
| `mysql_rs_retain()` → `ResultSet:` | `cache_save` |
| `mysql_rs_release(ResultSet:rs)` | `cache_delete` |
| `mysql_rs_activate(ResultSet:rs)` | `cache_set_active` |
| `mysql_rs_deactivate()` | `cache_unset_active` |
| `mysql_rs_is_active()` → `bool:` | `cache_is_any_active` |
| `mysql_rs_is_valid(ResultSet:rs)` → `bool:` | `cache_is_valid` |
| `mysql_rs_affected_rows()` | `cache_affected_rows` |
| `mysql_rs_insert_id()` | `cache_insert_id` |
| `mysql_rs_warning_count()` | `cache_warning_count` |
| `mysql_rs_exec_time(E_MYSQL_EXECTIME_UNIT:u=MICROSECONDS)` | `cache_get_query_exec_time` |
| `mysql_rs_query(dst[], len)` | `cache_get_query_string` |

Convenience stocks: `mysql_rs_num_rows()` / `mysql_rs_num_fields()` /
`mysql_rs_num_sets()` (replace `cache_num_rows`/`_fields`/`_results`), and the
`mysql_rs_value*` overload macros (replace `cache_get_value*`).

## Model (active record) — replaces the `orm_*` family

Bind globals to columns, set the key, then find / save / delete a row without
writing SQL. Bound variables must be **globals** (the model holds references
across the async call). Tag: `Model:`.

| omp-mySQL | replaces (SA-MP-MySQL) | notes |
|---|---|---|
| `mysql_model_create(table[], MySQL:h)` → `Model:` | `orm_create` | |
| `mysql_model_destroy(Model:m)` | `orm_destroy` | |
| `mysql_model_errno(Model:m)` → `E_MYSQL_MODEL_ERROR:` | `orm_errno` | |
| `mysql_model_bind_int(Model:m, &var, col[])` | `orm_addvar_int` | |
| `mysql_model_bind_float(Model:m, &Float:var, col[])` | `orm_addvar_float` | |
| `mysql_model_bind_string(Model:m, var[], maxlen, col[])` | `orm_addvar_string` | |
| `mysql_model_unbind(Model:m, col[])` | `orm_delvar` | |
| `mysql_model_clear(Model:m)` | `orm_clear_vars` | |
| `mysql_model_set_key(Model:m, col[])` | `orm_setkey` | |
| `mysql_model_find(Model:m, cb[]="", fmt[]="", ...)` | `orm_select` / `orm_load` | SELECT by key → bound vars. |
| `mysql_model_insert(Model:m, cb[]="", ...)` | `orm_insert` | auto-id written into the bound key. |
| `mysql_model_update(Model:m, cb[]="", ...)` | `orm_update` | |
| `mysql_model_save(Model:m, cb[]="", ...)` | `orm_save` | upsert: insert if key unset, else update. |
| `mysql_model_delete(Model:m, cb[]="", ...)` | `orm_delete` | |

`orm_apply_cache` has no direct equivalent — read result rows with the
`mysql_rs_*` family instead.

## Callbacks

| omp-mySQL | replaces | notes |
|---|---|---|
| `OnQueryError(errorid, error[], callback[], query[], MySQL:h)` | `OnQueryError` | also fires for guard-rejected queries. |
| `OnQueryExecute(MySQL:h, query[], exec_time)` | *(new)* | audit hook; `#define MYSQL_AUDIT` to enable. |

## Removed from the old plugin

| SA-MP-MySQL | why gone | do instead |
|---|---|---|
| "enable SSL" toggle / `mysql_ssl_set` | TLS is mandatory | use `SSL_MODE` / `SSL_CA` / `TLS_VERSION` config. |
| `mysql_native_password` reliance | removed in MySQL 9.x | `caching_sha2_password` (default). |
| gamemode-side `SHA256_PassHash` | insecure | `mysql_hash` (Argon2id). |
