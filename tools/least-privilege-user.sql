-- =============================================================================
--  omp-MySQL — least-privilege database user
--
--  Create a DB account that can ONLY do what the gamemode needs, so a leaked
--  config (hacked/exposed server files) can't be used to drop your database,
--  read other schemas, dump files, or pivot. This is the measure that actually
--  LIMITS DAMAGE — pair it with ${ENV} credentials and (optionally) an obfuscated
--  config. Mandatory TLS is enforced both by the component (fail-closed) and,
--  here, by the server (REQUIRE SSL).
--
--  Edit the placeholders, then run as root:
--     mysql -u root -p < tools/least-privilege-user.sql
--
--  WHAT THIS USER CANNOT DO (by omission — no GRANT for them):
--    - CREATE/DROP DATABASE, DROP TABLE on other schemas
--    - GRANT/CREATE USER (no privilege escalation)
--    - FILE (no LOAD_FILE / SELECT ... INTO OUTFILE reading server files)
--    - SUPER / PROCESS / SHUTDOWN / REPLICATION
--    - touch mysql.* or any database other than its own
-- =============================================================================

-- --- EDIT THESE --------------------------------------------------------------
SET @db    = 'ompdb';            -- the gamemode's schema (only one it can touch)
SET @user  = 'omp_app';          -- the application account
SET @pass  = 'CHANGE_ME_STRONG'; -- a long random password (store via ${ENV})
SET @host  = '10.0.0.0/255.0.0.0';-- WHERE it may connect from. Examples:
                                 --   'localhost'        same machine only
                                 --   '10.133.157.116'   one exact IP
                                 --   '10.0.0.0/255.0.0.0' a CIDR-style subnet
                                 -- AVOID '%' (anywhere) in production.

-- --- Schema (created if missing; utf8mb4 for full Unicode) --------------------
SET @sql = CONCAT('CREATE DATABASE IF NOT EXISTS `', @db,
  '` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- --- The user: TLS mandatory at the server too (REQUIRE SSL) ------------------
SET @sql = CONCAT('CREATE USER IF NOT EXISTS ''', @user, '''@''', @host,
  ''' IDENTIFIED BY ''', @pass, ''' REQUIRE SSL');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- Make sure an existing user is also forced onto TLS.
SET @sql = CONCAT('ALTER USER ''', @user, '''@''', @host, ''' REQUIRE SSL');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

-- --- Exactly the DML the gamemode needs, on ITS schema ONLY -------------------
-- SELECT/INSERT/UPDATE/DELETE cover persistence. EXECUTE is included only if you
-- use stored procedures; drop it otherwise. No DDL (CREATE/ALTER/DROP) — create
-- your tables as root/admin, not from the app account, in production.
SET @sql = CONCAT('GRANT SELECT, INSERT, UPDATE, DELETE ON `', @db,
  '`.* TO ''', @user, '''@''', @host, '''');
PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

FLUSH PRIVILEGES;

-- --- Verify ------------------------------------------------------------------
SELECT CONCAT('Created ', @user, '@', @host, ' limited to ', @db, '.* (REQUIRE SSL)') AS result;
-- Inspect what was granted:
--   SHOW GRANTS FOR 'omp_app'@'10.0.0.0/255.0.0.0';

-- =============================================================================
--  NOTE on row-level security: MySQL has NO built-in RLS (unlike PostgreSQL's
--  CREATE POLICY). It is also the WRONG tool here — the gamemode runs as a single
--  app user that legitimately manages every player's row, and it already scopes
--  every query with WHERE id=?/name=? (application-enforced row access). RLS
--  guards a trusted-but-limited user from seeing rows it shouldn't; that's not
--  this threat model. If you ever need column hiding, use column-level GRANTs,
--  e.g. GRANT SELECT (name, score) ON ompdb.accounts TO ...
-- =============================================================================
