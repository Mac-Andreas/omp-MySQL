/* =============================================================================
 *  omp-admin  -  the ULTIMATE save-everything admin/VIP demo for omp-mySQL.
 *
 *  Self-contained: needs only <open.mp> and <omp-mysql>. No zcmd / extra incs.
 *
 *  Persists (almost) everything about a player to MySQL and restores it on
 *  login: position + facing angle, interior, virtual world, last IP, money,
 *  score, health, armour, skin, colour, ALL 13 weapon slots (weapon + ammo),
 *  play time, login count, last-login timestamp, plus account fields (admin
 *  level, VIP, banned, muted, kills/deaths).
 *
 *  Exercises the whole omp-mySQL API over a TLS MySQL link:
 *    mysql_connect (mandatory TLS) · mysql_hash / mysql_verify (Argon2id) ·
 *    mysql_prepare + mysql_stmt_* (1-based) · mysql_model_* (active record) ·
 *    mysql_execute / mysql_rs_* · mysql_format (%e / %q).
 *
 *  Edit the DB_* settings below. The accounts table is created automatically.
 *
 *  Clean-room. MIT. Author: Xyranaut (Mac Andreas).
 * ========================================================================== */

#define FILTERSCRIPT

#include <open.mp>
#include <omp-mysql>

// --- DB connection (edit these) --------------------------------------------
#define DB_HOST     "127.0.0.1"
#define DB_USER     "omptest"
#define DB_PASS     "omptestpw"
#define DB_NAME     "ompdb"
#define DB_PORT     3306

#define AUTOSAVE_MS     120000   // periodic full save of every logged-in player
#define AUTH_TIMEOUT_MS  60000   // log in within this long or get kicked

// --- admin levels ----------------------------------------------------------
#define LEVEL_PLAYER   0
#define LEVEL_MOD      1
#define LEVEL_ADMIN    2
#define LEVEL_MANAGER  3
#define LEVEL_OWNER    4

// --- colours ---------------------------------------------------------------
#define COL_OK    0x66FF66FF
#define COL_ERR   0xFF6666FF
#define COL_INFO  0x33CCFFFF
#define COL_ADMIN 0xFFCC33FF
#define COL_GREY  0xCCCCCCFF

// --- dialog ids ------------------------------------------------------------
#define DLG_LOGIN      5200
#define DLG_REGISTER   5201
#define DLG_CMDS       5202
#define DLG_ADMINCMDS  5203
#define DLG_VIPCMDS    5204
#define DLG_MYSQL      5205

// --- per-player live session (everything we persist) -----------------------
enum E_ACC {
    Acc_Id,                 // DB row id (0 = not loaded)
    bool:Acc_Logged,
    Acc_Level,
    bool:Acc_Vip,
    bool:Acc_Banned,
    bool:Acc_Muted,
    bool:Acc_Frozen,
    Acc_Money,
    Acc_Score,
    Acc_Kills,
    Acc_Deaths,
    Acc_Skin,
    Acc_Colour,
    Acc_Interior,
    Acc_World,
    Float:Acc_X, Float:Acc_Y, Float:Acc_Z, Float:Acc_A,
    Float:Acc_Health, Float:Acc_Armour,
    Acc_Ip[24],
    Acc_Logins,
    Acc_PlayTime,           // seconds
    Acc_LoginTries,
    Acc_JoinTick,           // GetTickCount() at login (for playtime)
};
new g_acc[MAX_PLAYERS][E_ACC];

new MySQL:g_db = MYSQL_INVALID_HANDLE;
new bool:g_haveAccounts = false; // any account yet? (OWNER bootstrap)

// ===========================================================================
//  Small helpers (no `sizeof`-default params - that crashes pawncc 3.10)
// ===========================================================================

stock bool:IsNum(const s[])
{
    if (s[0] == '\0') return false;
    for (new i = 0; s[i]; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

stock ToLower(s[])
{
    for (new i = 0; s[i]; i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
}

stock Tok(const src[], &idx, dest[], destsize)
{
    new length = strlen(src), i = 0;
    while (idx < length && src[idx] <= ' ') idx++;
    while (idx < length && src[idx] > ' ' && i < (destsize - 1)) dest[i++] = src[idx++];
    dest[i] = '\0';
    return i;
}

stock LevelName(level)
{
    new s[12];
    switch (level)
    {
        case LEVEL_MOD:     s = "Moderator";
        case LEVEL_ADMIN:   s = "Admin";
        case LEVEL_MANAGER: s = "Manager";
        case LEVEL_OWNER:   s = "Owner";
        default:            s = "Player";
    }
    return s;
}

stock bool:RequireLevel(playerid, level)
{
    if (!g_acc[playerid][Acc_Logged]) { SendClientMessage(playerid, COL_ERR, "Log in first."); return false; }
    if (g_acc[playerid][Acc_Level] < level) { SendClientMessage(playerid, COL_ERR, "No permission."); return false; }
    return true;
}

stock FindTarget(const text[])
{
    if (IsNum(text))
    {
        new id = strval(text);
        return IsPlayerConnected(id) ? id : INVALID_PLAYER_ID;
    }
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (!IsPlayerConnected(i)) continue;
        new pn[MAX_PLAYER_NAME];
        GetPlayerName(i, pn, sizeof pn);
        if (strfind(pn, text, true) != -1) return i;
    }
    return INVALID_PLAYER_ID;
}

// ===========================================================================
//  Capture / restore the FULL live player state
// ===========================================================================

// Snapshot everything from the live player into the session struct.
SnapshotPlayer(playerid)
{
    new Float:x, Float:y, Float:z, Float:a, Float:hp, Float:ar;
    GetPlayerPos(playerid, x, y, z);
    GetPlayerFacingAngle(playerid, a);
    GetPlayerHealth(playerid, hp);
    GetPlayerArmour(playerid, ar);
    g_acc[playerid][Acc_X] = x; g_acc[playerid][Acc_Y] = y;
    g_acc[playerid][Acc_Z] = z; g_acc[playerid][Acc_A] = a;
    g_acc[playerid][Acc_Health] = hp; g_acc[playerid][Acc_Armour] = ar;
    g_acc[playerid][Acc_Money]    = GetPlayerMoney(playerid);
    g_acc[playerid][Acc_Score]    = GetPlayerScore(playerid);
    g_acc[playerid][Acc_Skin]     = GetPlayerSkin(playerid);
    g_acc[playerid][Acc_Colour]   = GetPlayerColor(playerid);
    g_acc[playerid][Acc_Interior] = GetPlayerInterior(playerid);
    g_acc[playerid][Acc_World]    = GetPlayerVirtualWorld(playerid);
    // playtime: accumulate since last login mark
    new now = GetTickCount();
    if (g_acc[playerid][Acc_JoinTick] != 0)
        g_acc[playerid][Acc_PlayTime] += (now - g_acc[playerid][Acc_JoinTick]) / 1000;
    g_acc[playerid][Acc_JoinTick] = now;
}

// Apply the saved session back onto the live player after login.
RestorePlayer(playerid)
{
    SetPlayerPos(playerid, g_acc[playerid][Acc_X], g_acc[playerid][Acc_Y], g_acc[playerid][Acc_Z]);
    SetPlayerFacingAngle(playerid, g_acc[playerid][Acc_A]);
    SetPlayerInterior(playerid, g_acc[playerid][Acc_Interior]);
    SetPlayerVirtualWorld(playerid, g_acc[playerid][Acc_World]);
    if (g_acc[playerid][Acc_Health] > 0.0) SetPlayerHealth(playerid, g_acc[playerid][Acc_Health]);
    SetPlayerArmour(playerid, g_acc[playerid][Acc_Armour]);
    GivePlayerMoney(playerid, g_acc[playerid][Acc_Money] - GetPlayerMoney(playerid));
    SetPlayerScore(playerid, g_acc[playerid][Acc_Score]);
    if (g_acc[playerid][Acc_Skin] > 0) SetPlayerSkin(playerid, g_acc[playerid][Acc_Skin]);
    if (g_acc[playerid][Acc_Colour] != 0) SetPlayerColor(playerid, g_acc[playerid][Acc_Colour]);
}

// Persist the FULL state to the account row (prepared statement, injection-safe).
SavePlayer(playerid)
{
    if (!g_acc[playerid][Acc_Logged] || g_acc[playerid][Acc_Id] == 0) return;
    SnapshotPlayer(playerid);

    new PreparedStatement:st = mysql_prepare(g_db,
        "UPDATE accounts SET money=?, score=?, kills=?, deaths=?, level=?, vip=?, \
banned=?, muted=?, skin=?, colour=?, interior=?, world=?, x=?, y=?, z=?, a=?, \
health=?, armour=?, ip=?, logins=?, playtime=? WHERE id=?");
    mysql_stmt_set_int(st, 1,  g_acc[playerid][Acc_Money]);
    mysql_stmt_set_int(st, 2,  g_acc[playerid][Acc_Score]);
    mysql_stmt_set_int(st, 3,  g_acc[playerid][Acc_Kills]);
    mysql_stmt_set_int(st, 4,  g_acc[playerid][Acc_Deaths]);
    mysql_stmt_set_int(st, 5,  g_acc[playerid][Acc_Level]);
    mysql_stmt_set_int(st, 6,  g_acc[playerid][Acc_Vip] ? 1 : 0);
    mysql_stmt_set_int(st, 7,  g_acc[playerid][Acc_Banned] ? 1 : 0);
    mysql_stmt_set_int(st, 8,  g_acc[playerid][Acc_Muted] ? 1 : 0);
    mysql_stmt_set_int(st, 9,  g_acc[playerid][Acc_Skin]);
    mysql_stmt_set_int(st, 10, g_acc[playerid][Acc_Colour]);
    mysql_stmt_set_int(st, 11, g_acc[playerid][Acc_Interior]);
    mysql_stmt_set_int(st, 12, g_acc[playerid][Acc_World]);
    mysql_stmt_set_float(st, 13, g_acc[playerid][Acc_X]);
    mysql_stmt_set_float(st, 14, g_acc[playerid][Acc_Y]);
    mysql_stmt_set_float(st, 15, g_acc[playerid][Acc_Z]);
    mysql_stmt_set_float(st, 16, g_acc[playerid][Acc_A]);
    mysql_stmt_set_float(st, 17, g_acc[playerid][Acc_Health]);
    mysql_stmt_set_float(st, 18, g_acc[playerid][Acc_Armour]);
    mysql_stmt_set_string(st, 19, g_acc[playerid][Acc_Ip]);
    mysql_stmt_set_int(st, 20, g_acc[playerid][Acc_Logins]);
    mysql_stmt_set_int(st, 21, g_acc[playerid][Acc_PlayTime]);
    mysql_stmt_set_int(st, 22, g_acc[playerid][Acc_Id]);
    mysql_stmt_execute(st);
    mysql_stmt_close(st);

    // Weapons: a separate child table, replaced each save.
    new wq[96];
    mysql_format(g_db, wq, sizeof wq, "DELETE FROM account_weapons WHERE account_id=%d", g_acc[playerid][Acc_Id]);
    mysql_execute(g_db, wq);
    for (new slot = 0; slot < 13; slot++)
    {
        new WEAPON:wep; new ammo;
        GetPlayerWeaponData(playerid, WEAPON_SLOT:slot, wep, ammo);
        if (_:wep == 0 || ammo == 0) continue;
        new q2[128];
        mysql_format(g_db, q2, sizeof q2,
            "INSERT INTO account_weapons (account_id, slot, weapon, ammo) VALUES (%d, %d, %d, %d)",
            g_acc[playerid][Acc_Id], slot, _:wep, ammo);
        mysql_execute(g_db, q2);
    }
}

// ===========================================================================
//  Lifecycle + schema
// ===========================================================================

public OnFilterScriptInit()
{
    print("[omp-admin] starting...");

    new MySQLConfig:cfg = mysql_config_create();
    mysql_config_set(cfg, SSL_MODE, SSL_MODE_REQUIRED);
    mysql_config_set(cfg, SERVER_PORT, DB_PORT);
    g_db = mysql_connect(DB_HOST, DB_USER, DB_PASS, DB_NAME, cfg);
    if (g_db == MYSQL_INVALID_HANDLE)
    {
        printf("[omp-admin] DB connect FAILED (errno %d).", mysql_errno(g_db));
        return 1;
    }
    new cipher[64];
    mysql_get_tls_cipher(cipher, sizeof cipher, g_db);
    printf("[omp-admin] connected over TLS (%s).", cipher);

    mysql_execute_sync(g_db,
        "CREATE TABLE IF NOT EXISTS accounts (\
id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(24) NOT NULL UNIQUE, hash VARCHAR(255) NOT NULL,\
level INT NOT NULL DEFAULT 0, vip INT NOT NULL DEFAULT 0, banned INT NOT NULL DEFAULT 0,\
muted INT NOT NULL DEFAULT 0, money INT NOT NULL DEFAULT 0, score INT NOT NULL DEFAULT 0,\
kills INT NOT NULL DEFAULT 0, deaths INT NOT NULL DEFAULT 0, skin INT NOT NULL DEFAULT 0,\
colour INT NOT NULL DEFAULT 0, interior INT NOT NULL DEFAULT 0, world INT NOT NULL DEFAULT 0,\
x FLOAT NOT NULL DEFAULT 0, y FLOAT NOT NULL DEFAULT 0, z FLOAT NOT NULL DEFAULT 0,\
a FLOAT NOT NULL DEFAULT 0, health FLOAT NOT NULL DEFAULT 100, armour FLOAT NOT NULL DEFAULT 0,\
ip VARCHAR(45) NOT NULL DEFAULT '', logins INT NOT NULL DEFAULT 0, playtime INT NOT NULL DEFAULT 0,\
last_login DATETIME NULL, created DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP\
) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci");

    mysql_execute_sync(g_db,
        "CREATE TABLE IF NOT EXISTS account_weapons (\
account_id INT NOT NULL, slot INT NOT NULL, weapon INT NOT NULL, ammo INT NOT NULL,\
PRIMARY KEY (account_id, slot)) CHARACTER SET utf8mb4");

    // Any accounts yet? (first registration becomes OWNER).
    mysql_execute(g_db, "SELECT COUNT(*) AS c FROM accounts", "OnCountAccounts");

    // If the filterscript was RELOADED with players already online, their session
    // memory is gone (everyone is logged out) -> re-run auth so they're prompted
    // again. Without this they'd be stuck on "Log in first" with no dialog.
    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i)) BeginAuth(i);

    SetTimer("AutoSaveAll", AUTOSAVE_MS, true);
    print("[omp-admin] loaded.");
    return 1;
}

forward OnCountAccounts();
public OnCountAccounts()
{
    new c;
    mysql_rs_get_int_by(0, "c", c);
    g_haveAccounts = (c > 0);
    return 1;
}

public OnFilterScriptExit()
{
    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i) && g_acc[i][Acc_Logged]) SavePlayer(i);
    if (g_db != MYSQL_INVALID_HANDLE) mysql_close(g_db);
    return 1;
}

forward AutoSaveAll();
public AutoSaveAll()
{
    new saved = 0;
    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i) && g_acc[i][Acc_Logged]) { SavePlayer(i); saved++; }
    if (saved) printf("[omp-admin] autosaved %d player(s).", saved);
    return 1;
}

// Start (or restart) authentication for a player: UNCONDITIONALLY wipe the session
// to a clean logged-OUT state, then look the account up by NAME and show
// Login/Register. Called on every connect AND for already-connected players when
// the filterscript (re)loads. Always wiping is the security keystone: a rejoining
// player (or a reused slot id) is NEVER treated as already-logged-in, so a
// password is always required — no auto-login, no impersonation.
BeginAuth(playerid)
{
    for (new k = 0; k < _:E_ACC; k++) g_acc[playerid][E_ACC:k] = 0;
    g_acc[playerid][Acc_Logged] = false;
    GetPlayerIp(playerid, g_acc[playerid][Acc_Ip], 24);

    // Account lookup by name (prepared statement).
    new name[MAX_PLAYER_NAME];
    GetPlayerName(playerid, name, sizeof name);
    new PreparedStatement:st = mysql_prepare(g_db, "SELECT id FROM accounts WHERE name = ? LIMIT 1");
    mysql_stmt_set_string(st, 1, name);
    SetPVarInt(playerid, "lookupStmt", _:st);
    mysql_stmt_execute(st, "OnAccountLookup", "d", playerid);

    // Hard deadline: if they haven't logged in within AUTH_TIMEOUT_MS (escaped the
    // prompt, idled, or never opened it), drop them. Re-armed each BeginAuth.
    SetTimerEx("AuthTimeout", AUTH_TIMEOUT_MS, false, "d", playerid);
}

forward AuthTimeout(playerid);
public AuthTimeout(playerid)
{
    if (IsPlayerConnected(playerid) && !g_acc[playerid][Acc_Logged])
    {
        SendClientMessage(playerid, COL_ERR, "Login timed out. Reconnect and log in to play.");
        Kick(playerid);
    }
    return 1;
}

public OnPlayerConnect(playerid)
{
    BeginAuth(playerid);
    return 1;
}

forward OnAccountLookup(playerid);
public OnAccountLookup(playerid)
{
    new PreparedStatement:st = PreparedStatement:GetPVarInt(playerid, "lookupStmt");
    mysql_stmt_close(st);
    DeletePVar(playerid, "lookupStmt");

    // Freeze the player until they authenticate, so an unlogged player can't
    // roam/shoot even if they dismiss the dialog. Unfrozen on successful login.
    TogglePlayerControllable(playerid, false);

    new rows;
    mysql_rs_row_count(rows);
    if (rows > 0)
    {
        new id;
        mysql_rs_get_int_by(0, "id", id);
        g_acc[playerid][Acc_Id] = id;
        ShowPlayerDialog(playerid, DLG_LOGIN, DIALOG_STYLE_PASSWORD,
            "omp-admin - Login", "Welcome back.\nEnter your password:", "Login", "Quit");
    }
    else
    {
        ShowPlayerDialog(playerid, DLG_REGISTER, DIALOG_STYLE_PASSWORD,
            "omp-admin - Register", "New here.\nChoose a password (4+ chars):", "Register", "Quit");
    }
    return 1;
}

// If an unauthenticated player spawns (or the class-select screen lets them
// through), re-show the login prompt and keep them frozen. Belt-and-suspenders
// with the auth timeout + cancel-kick so there is no unauthenticated play.
public OnPlayerSpawn(playerid)
{
    if (!g_acc[playerid][Acc_Logged])
    {
        TogglePlayerControllable(playerid, false);
        BeginAuth(playerid);
    }
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    #pragma unused reason
    if (g_acc[playerid][Acc_Logged]) SavePlayer(playerid);
    // CRITICAL: clear the slot's session so the NEXT player to reuse this slot id
    // (or this same player rejoining) is never treated as already-logged-in.
    for (new k = 0; k < _:E_ACC; k++) g_acc[playerid][E_ACC:k] = 0;
    g_acc[playerid][Acc_Logged] = false;
    return 1;
}

// On successful RCON login, greet with the omp-MySQL version and - if a newer
// release is known - an "[omp-MySQL]: New version available" update notice.
public OnRconLoginAttempt(ip[], password[], success)
{
    #pragma unused ip, password
    if (!success) return 1;
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (!IsPlayerConnected(i) || !IsPlayerAdmin(i)) continue;
        new ver[24], msg[160];
        mysql_version(ver, sizeof ver);
        if (mysql_update_available())
        {
            new latest[24];
            mysql_latest_version(latest, sizeof latest);
            format(msg, sizeof msg,
                "[omp-MySQL]: New version available %s (current: %s). Update to keep your server secure.",
                latest, ver);
            SendClientMessage(i, COL_ADMIN, msg);
        }
        else
        {
            format(msg, sizeof msg, "[omp-MySQL]: v%s - up to date. Type /mysql for diagnostics.", ver);
            SendClientMessage(i, COL_INFO, msg);
        }
    }
    return 1;
}

// ===========================================================================
//  Auth - Argon2id hashing + full-row load on login
// ===========================================================================

public OnDialogResponse(playerid, dialogid, response, listitem, inputtext[])
{
    // Already-authenticated players can't re-trigger the auth flow (no double
    // register / re-login replay). Swallow any stale/forged auth dialog response.
    if ((dialogid == DLG_LOGIN || dialogid == DLG_REGISTER) && g_acc[playerid][Acc_Logged])
    {
        return 1;
    }
    if (dialogid == DLG_LOGIN)
    {
        // Cancel/ESC on a mandatory auth prompt = you don't get to play. Kick.
        if (!response) { KickForNoAuth(playerid); return 1; }
        SetPVarString(playerid, "pw", inputtext);
        // Load the full account row by id, then verify the password.
        new q[96];
        mysql_format(g_db, q, sizeof q, "SELECT * FROM accounts WHERE id=%d LIMIT 1", g_acc[playerid][Acc_Id]);
        mysql_execute(g_db, q, "OnAccountLoaded", "d", playerid);
        return 1;
    }
    if (dialogid == DLG_REGISTER)
    {
        if (!response) { KickForNoAuth(playerid); return 1; }
        if (strlen(inputtext) < 4)
        {
            ShowPlayerDialog(playerid, DLG_REGISTER, DIALOG_STYLE_PASSWORD,
                "omp-admin - Register", "Too short - choose a password (4+ chars):", "Register", "Quit");
            return 1;
        }
        mysql_hash(inputtext, "OnPasswordHashed", "d", HASH_ARGON2ID, playerid);
        return 1;
    }
    return 0;
}

// Kick a player who escaped/cancelled a mandatory login/register prompt. The
// message + a brief delay give the client time to show it before the drop.
KickForNoAuth(playerid)
{
    if (!IsPlayerConnected(playerid)) return;
    SendClientMessage(playerid, COL_ERR, "You must log in to play here. Reconnect and enter your password.");
    SetTimerEx("DelayedKick", 300, false, "d", playerid);
}

forward DelayedKick(playerid);
public DelayedKick(playerid)
{
    if (IsPlayerConnected(playerid) && !g_acc[playerid][Acc_Logged]) Kick(playerid);
    return 1;
}

// Register: password hashed → INSERT a fresh account (prepared statement).
forward OnPasswordHashed(playerid, const hash[]);
public OnPasswordHashed(playerid, const hash[])
{
    new level = LEVEL_PLAYER;
    if (!g_haveAccounts) { level = LEVEL_OWNER; g_haveAccounts = true; }

    new name[MAX_PLAYER_NAME];
    GetPlayerName(playerid, name, sizeof name);
    new PreparedStatement:st = mysql_prepare(g_db,
        "INSERT INTO accounts (name, hash, level, ip, last_login, logins) VALUES (?, ?, ?, ?, NOW(), 1)");
    mysql_stmt_set_string(st, 1, name);
    mysql_stmt_set_string(st, 2, hash);
    mysql_stmt_set_int(st, 3, level);
    mysql_stmt_set_string(st, 4, g_acc[playerid][Acc_Ip]);
    SetPVarInt(playerid, "regStmt", _:st);
    SetPVarInt(playerid, "regLevel", level);
    mysql_stmt_execute(st, "OnAccountInserted", "d", playerid);
    return 1;
}

forward OnAccountInserted(playerid);
public OnAccountInserted(playerid)
{
    new PreparedStatement:st = PreparedStatement:GetPVarInt(playerid, "regStmt");
    mysql_stmt_close(st);
    DeletePVar(playerid, "regStmt");

    g_acc[playerid][Acc_Id]     = mysql_rs_insert_id();
    g_acc[playerid][Acc_Logged] = true;
    g_acc[playerid][Acc_Level]  = GetPVarInt(playerid, "regLevel");
    g_acc[playerid][Acc_Health] = 100.0;
    g_acc[playerid][Acc_Logins] = 1;
    g_acc[playerid][Acc_JoinTick] = GetTickCount();
    DeletePVar(playerid, "regLevel");

    TogglePlayerControllable(playerid, true); // authenticated -> unfreeze
    SendClientMessage(playerid, COL_OK, "Account registered - you are logged in.");
    if (g_acc[playerid][Acc_Level] == LEVEL_OWNER)
        SendClientMessage(playerid, COL_ADMIN, "You are the server OWNER (first account).");
    return 1;
}

// Login: full row loaded → verify password, then hydrate + restore the player.
forward OnAccountLoaded(playerid);
public OnAccountLoaded(playerid)
{
    new rows;
    mysql_rs_row_count(rows);
    if (rows == 0) { SendClientMessage(playerid, COL_ERR, "Account load failed."); Kick(playerid); return 1; }

    new hash[256], pw[64];
    mysql_rs_get_string_by(0, "hash", hash, sizeof hash);
    GetPVarString(playerid, "pw", pw, sizeof pw);
    DeletePVar(playerid, "pw");

    if (!mysql_verify_sync(pw, hash))
    {
        if (++g_acc[playerid][Acc_LoginTries] >= 3) { SendClientMessage(playerid, COL_ERR, "Too many wrong passwords."); Kick(playerid); }
        else ShowPlayerDialog(playerid, DLG_LOGIN, DIALOG_STYLE_PASSWORD, "omp-admin - Login", "Wrong password. Try again:", "Login", "Quit");
        return 1;
    }

    // Hydrate the whole session from the row.
    g_acc[playerid][Acc_Logged] = true;
    TogglePlayerControllable(playerid, true); // authenticated -> unfreeze
    mysql_rs_get_int_by(0, "level",  g_acc[playerid][Acc_Level]);
    new tmp;
    mysql_rs_get_int_by(0, "vip", tmp);    g_acc[playerid][Acc_Vip]    = (tmp != 0);
    mysql_rs_get_int_by(0, "banned", tmp); g_acc[playerid][Acc_Banned] = (tmp != 0);
    mysql_rs_get_int_by(0, "muted", tmp);  g_acc[playerid][Acc_Muted]  = (tmp != 0);
    mysql_rs_get_int_by(0, "money",    g_acc[playerid][Acc_Money]);
    mysql_rs_get_int_by(0, "score",    g_acc[playerid][Acc_Score]);
    mysql_rs_get_int_by(0, "kills",    g_acc[playerid][Acc_Kills]);
    mysql_rs_get_int_by(0, "deaths",   g_acc[playerid][Acc_Deaths]);
    mysql_rs_get_int_by(0, "skin",     g_acc[playerid][Acc_Skin]);
    mysql_rs_get_int_by(0, "colour",   g_acc[playerid][Acc_Colour]);
    mysql_rs_get_int_by(0, "interior", g_acc[playerid][Acc_Interior]);
    mysql_rs_get_int_by(0, "world",    g_acc[playerid][Acc_World]);
    mysql_rs_get_float_by(0, "x", g_acc[playerid][Acc_X]);
    mysql_rs_get_float_by(0, "y", g_acc[playerid][Acc_Y]);
    mysql_rs_get_float_by(0, "z", g_acc[playerid][Acc_Z]);
    mysql_rs_get_float_by(0, "a", g_acc[playerid][Acc_A]);
    mysql_rs_get_float_by(0, "health", g_acc[playerid][Acc_Health]);
    mysql_rs_get_float_by(0, "armour", g_acc[playerid][Acc_Armour]);
    mysql_rs_get_int_by(0, "logins",   g_acc[playerid][Acc_Logins]);
    mysql_rs_get_int_by(0, "playtime", g_acc[playerid][Acc_PlayTime]);
    g_acc[playerid][Acc_Logins]++;
    g_acc[playerid][Acc_JoinTick] = GetTickCount();

    if (g_acc[playerid][Acc_Banned]) { SendClientMessage(playerid, COL_ERR, "This account is banned."); Kick(playerid); return 1; }

    // bump last_login
    new uq[96];
    mysql_format(g_db, uq, sizeof uq, "UPDATE accounts SET last_login=NOW(), logins=%d WHERE id=%d",
        g_acc[playerid][Acc_Logins], g_acc[playerid][Acc_Id]);
    mysql_execute(g_db, uq);

    RestorePlayer(playerid);
    // Restore weapons from the child table.
    new wq[96];
    mysql_format(g_db, wq, sizeof wq, "SELECT weapon, ammo FROM account_weapons WHERE account_id=%d", g_acc[playerid][Acc_Id]);
    mysql_execute(g_db, wq, "OnWeaponsLoaded", "d", playerid);

    SendClientMessage(playerid, COL_OK, "Logged in - your session was restored.");
    new msg[128];
    format(msg, sizeof msg, "Level: %s%s | $%d | Score: %d | Logins: %d | Playtime: %dm",
        LevelName(g_acc[playerid][Acc_Level]), g_acc[playerid][Acc_Vip] ? " (VIP)" : "",
        g_acc[playerid][Acc_Money], g_acc[playerid][Acc_Score], g_acc[playerid][Acc_Logins],
        g_acc[playerid][Acc_PlayTime] / 60);
    SendClientMessage(playerid, COL_INFO, msg);
    return 1;
}

forward OnWeaponsLoaded(playerid);
public OnWeaponsLoaded(playerid)
{
    new rows;
    mysql_rs_row_count(rows);
    for (new r = 0; r < rows; r++)
    {
        new wep, ammo;
        mysql_rs_get_int_by(r, "weapon", wep);
        mysql_rs_get_int_by(r, "ammo", ammo);
        if (wep > 0 && ammo > 0) GivePlayerWeapon(playerid, WEAPON:wep, ammo);
    }
    return 1;
}

// ===========================================================================
//  Commands (native dispatcher - no zcmd)
// ===========================================================================

public OnPlayerCommandText(playerid, cmdtext[])
{
    new cmd[32], idx = 0;
    Tok(cmdtext, idx, cmd, sizeof cmd);
    ToLower(cmd);
    new params[128], pidx = idx;
    new plen = 0, L = strlen(cmdtext);
    while (pidx < L && cmdtext[pidx] <= ' ') pidx++;
    while (pidx < L && plen < (sizeof(params) - 1)) params[plen++] = cmdtext[pidx++];
    params[plen] = '\0';

    // /mysql is an RCON-only diagnostics command - available BEFORE login so it
    // can be used to debug the very login/DB path. Gated on IsPlayerAdmin (RCON).
    if (!strcmp(cmd, "/mysql", true)) return Cmd_MySQL(playerid, params);

    if (!g_acc[playerid][Acc_Logged]) { SendClientMessage(playerid, COL_ERR, "Log in first."); return 1; }

    if (!strcmp(cmd, "/cmds", true))   return Cmd_Cmds(playerid);
    if (!strcmp(cmd, "/help", true))   return Cmd_Help(playerid);
    if (!strcmp(cmd, "/stats", true))  return Cmd_Stats(playerid, params);
    if (!strcmp(cmd, "/admins", true)) return Cmd_Admins(playerid);
    if (!strcmp(cmd, "/save", true))   { SavePlayer(playerid); SendClientMessage(playerid, COL_OK, "Saved."); return 1; }

    if (!strcmp(cmd, "/vcmds", true))  return Cmd_VipCmds(playerid);
    if (!strcmp(cmd, "/heal", true))   return Cmd_Heal(playerid);

    if (!strcmp(cmd, "/acmds", true))    return Cmd_AdminCmds(playerid);
    if (!strcmp(cmd, "/kick", true))     return Cmd_Kick(playerid, params);
    if (!strcmp(cmd, "/ban", true))      return Cmd_Ban(playerid, params);
    if (!strcmp(cmd, "/unban", true))    return Cmd_Unban(playerid, params);
    if (!strcmp(cmd, "/mute", true))     return Cmd_Mute(playerid, params, true);
    if (!strcmp(cmd, "/unmute", true))   return Cmd_Mute(playerid, params, false);
    if (!strcmp(cmd, "/freeze", true))   return Cmd_Freeze(playerid, params, true);
    if (!strcmp(cmd, "/unfreeze", true)) return Cmd_Freeze(playerid, params, false);
    if (!strcmp(cmd, "/goto", true))     return Cmd_Goto(playerid, params);
    if (!strcmp(cmd, "/gethere", true))  return Cmd_GetHere(playerid, params);
    if (!strcmp(cmd, "/givecash", true)) return Cmd_GiveCash(playerid, params);
    if (!strcmp(cmd, "/setlevel", true)) return Cmd_SetLevel(playerid, params);
    if (!strcmp(cmd, "/setvip", true))   return Cmd_SetVip(playerid, params);
    if (!strcmp(cmd, "/setskin", true))  return Cmd_SetSkin(playerid, params);

    SendClientMessage(playerid, COL_ERR, "Unknown command. /cmds for the list.");
    return 1;
}

// RCON-only MySQL diagnostics. Usage:
//   /mysql                  - overview + sub-command help
//   /mysql status           - connection handle, TLS cipher, errno/error
//   /mysql version          - server version (SELECT VERSION())
//   /mysql debug on|off [f] - toggle [omp-MySQL] component debug logging (+ file)
//   /mysql test             - run a trivial query to prove the link works
Cmd_MySQL(playerid, const params[])
{
    if (!IsPlayerAdmin(playerid))
    {
        SendClientMessage(playerid, COL_ERR, "/mysql is RCON-only. Log in to RCON first (/rcon login).");
        return 1;
    }

    new sub[16], idx = 0;
    Tok(params, idx, sub, sizeof sub);
    ToLower(sub);

    if (!sub[0])
    {
        // MSGBOX overview: centred "omp-MySQL <ver>" title (+ update note), then the
        // sub-commands. Built as one dialog body rather than scattered chat lines.
        new ver[24], latest[24], title[32], body[512], line[96];
        mysql_version(ver, sizeof ver);
        format(title, sizeof title, "omp-MySQL %s", ver);
        if (mysql_update_available())
        {
            mysql_latest_version(latest, sizeof latest);
            format(line, sizeof line, "{FFCC33}Update available: %s{FFFFFF}\n\n", latest);
            strcat(body, line);
        }
        else
        {
            strcat(body, "{66FF66}Up to date.{FFFFFF}\n\n");
        }
        strcat(body, "{33CCFF}RCON diagnostics{FFFFFF}\n");
        strcat(body, "/mysql status - connection handle + TLS cipher\n");
        strcat(body, "/mysql version - MySQL server version\n");
        strcat(body, "/mysql debug on|off [file] - [omp-MySQL] logging\n");
        strcat(body, "/mysql test - run a probe query\n");
        ShowPlayerDialog(playerid, DLG_MYSQL, DIALOG_STYLE_MSGBOX, title, body, "Close", "");
        return 1;
    }

    if (!strcmp(sub, "status", true))
    {
        new msg[160], cipher[64];
        mysql_get_tls_cipher(cipher, sizeof cipher, g_db);
        format(msg, sizeof msg, "handle=%d  TLS=%s  errno=%d",
            _:g_db, cipher[0] ? cipher : "(none!)", mysql_errno(g_db));
        SendClientMessage(playerid, COL_OK, msg);
        new err[100];
        mysql_error(err, sizeof err, g_db);
        if (err[0]) { format(msg, sizeof msg, "last error: %s", err); SendClientMessage(playerid, COL_GREY, msg); }
        return 1;
    }

    if (!strcmp(sub, "version", true))
    {
        mysql_execute(g_db, "SELECT VERSION() AS v", "OnMySQLVersion", "d", playerid);
        SendClientMessage(playerid, COL_GREY, "Querying server version…");
        return 1;
    }

    if (!strcmp(sub, "debug", true))
    {
        new arg[16]; new j = idx;
        Tok(params, j, arg, sizeof arg);
        ToLower(arg);
        new file[96]; Tok(params, j, file, sizeof file);
        if (!strcmp(arg, "on", true))
        {
            mysql_debug(true, file);
            new msg[140];
            if (file[0]) format(msg, sizeof msg, "[omp-MySQL] debug ON (console + file '%s')", file);
            else         format(msg, sizeof msg, "[omp-MySQL] debug ON (console)");
            SendClientMessage(playerid, COL_OK, msg);
        }
        else if (!strcmp(arg, "off", true))
        {
            mysql_debug(false);
            SendClientMessage(playerid, COL_OK, "[omp-MySQL] debug OFF");
        }
        else
        {
            new msg[80];
            format(msg, sizeof msg, "debug is %s. Use: /mysql debug on|off [file]",
                mysql_debug_enabled() ? "ON" : "OFF");
            SendClientMessage(playerid, COL_GREY, msg);
        }
        return 1;
    }

    if (!strcmp(sub, "test", true))
    {
        mysql_execute(g_db, "SELECT 1+1 AS answer", "OnMySQLTest", "d", playerid);
        SendClientMessage(playerid, COL_GREY, "Running probe query (1+1)…");
        return 1;
    }

    SendClientMessage(playerid, COL_ERR, "Unknown /mysql sub-command. Type /mysql for help.");
    return 1;
}

forward OnMySQLVersion(playerid);
public OnMySQLVersion(playerid)
{
    new v[64], msg[100];
    mysql_rs_get_string_by(0, "v", v, sizeof v);
    format(msg, sizeof msg, "MySQL server version: %s", v[0] ? v : "(unknown)");
    SendClientMessage(playerid, COL_OK, msg);
    return 1;
}

forward OnMySQLTest(playerid);
public OnMySQLTest(playerid)
{
    new ans, msg[80];
    mysql_rs_get_int_by(0, "answer", ans);
    format(msg, sizeof msg, "Probe OK: SELECT 1+1 = %d (link is healthy).", ans);
    SendClientMessage(playerid, COL_OK, msg);
    return 1;
}

Cmd_Cmds(playerid)
{
    new list[400];
    strcat(list, "Stats - your saved account\n");
    strcat(list, "Help\nAdmins online\nSave now\n");
    if (g_acc[playerid][Acc_Vip]) strcat(list, "VIP commands\n");
    if (g_acc[playerid][Acc_Level] >= LEVEL_MOD) strcat(list, "Admin commands\n");
    ShowPlayerDialog(playerid, DLG_CMDS, DIALOG_STYLE_LIST, "omp-admin - Commands", list, "Select", "Close");
    return 1;
}

Cmd_Help(playerid)
{
    SendClientMessage(playerid, COL_INFO, "omp-admin: full save-everything demo on omp-mySQL.");
    SendClientMessage(playerid, COL_GREY, "Your position, money, score, skin, weapons, playtime & more persist to MySQL.");
    SendClientMessage(playerid, COL_GREY, "Argon2id passwords over a TLS link. Type /cmds.");
    return 1;
}

Cmd_Stats(playerid, const params[])
{
    new target = playerid;
    if (params[0] && RequireLevel(playerid, LEVEL_MOD))
    {
        target = FindTarget(params);
        if (target == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Player not found."); return 1; }
    }
    SnapshotPlayer(target);
    new m[144];
    format(m, sizeof m, "%s - %s%s | $%d | Score %d | K/D %d/%d",
        GetPlayerNameEx(target), LevelName(g_acc[target][Acc_Level]),
        g_acc[target][Acc_Vip] ? " VIP" : "", g_acc[target][Acc_Money],
        g_acc[target][Acc_Score], g_acc[target][Acc_Kills], g_acc[target][Acc_Deaths]);
    SendClientMessage(playerid, COL_INFO, m);
    format(m, sizeof m, "Skin %d | Pos %.0f,%.0f,%.0f | Int %d VW %d | Playtime %dm | Logins %d | IP %s",
        g_acc[target][Acc_Skin], g_acc[target][Acc_X], g_acc[target][Acc_Y], g_acc[target][Acc_Z],
        g_acc[target][Acc_Interior], g_acc[target][Acc_World], g_acc[target][Acc_PlayTime]/60,
        g_acc[target][Acc_Logins], g_acc[target][Acc_Ip]);
    SendClientMessage(playerid, COL_GREY, m);
    return 1;
}

Cmd_Admins(playerid)
{
    SendClientMessage(playerid, COL_ADMIN, "Online staff:");
    new found = 0;
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (!IsPlayerConnected(i) || !g_acc[i][Acc_Logged] || g_acc[i][Acc_Level] < LEVEL_MOD) continue;
        new m[96];
        format(m, sizeof m, "  %s (id %d) - %s", GetPlayerNameEx(i), i, LevelName(g_acc[i][Acc_Level]));
        SendClientMessage(playerid, COL_GREY, m);
        found++;
    }
    if (!found) SendClientMessage(playerid, COL_GREY, "  (none online)");
    return 1;
}

Cmd_VipCmds(playerid)
{
    if (!g_acc[playerid][Acc_Vip] && g_acc[playerid][Acc_Level] < LEVEL_ADMIN) { SendClientMessage(playerid, COL_ERR, "VIP only."); return 1; }
    ShowPlayerDialog(playerid, DLG_VIPCMDS, DIALOG_STYLE_LIST, "omp-admin - VIP", "/heal - full health & armour\n", "Close", "");
    return 1;
}

Cmd_Heal(playerid)
{
    if (!g_acc[playerid][Acc_Vip] && g_acc[playerid][Acc_Level] < LEVEL_ADMIN) { SendClientMessage(playerid, COL_ERR, "VIP only."); return 1; }
    SetPlayerHealth(playerid, 100.0); SetPlayerArmour(playerid, 100.0);
    SendClientMessage(playerid, COL_OK, "Healed.");
    return 1;
}

Cmd_AdminCmds(playerid)
{
    if (!RequireLevel(playerid, LEVEL_MOD)) return 1;
    // MSGBOX dialog listing every admin command grouped by the level it needs.
    new body[900];
    strcat(body, "{FFCC33}Moderator (1+){FFFFFF}\n");
    strcat(body, "/kick <id> - kick a player\n");
    strcat(body, "/ban <id> - ban a player\n");
    strcat(body, "/unban <name> - lift a ban\n");
    strcat(body, "/mute <id> - mute a player\n");
    strcat(body, "/unmute <id> - unmute a player\n");
    strcat(body, "/freeze <id> - freeze a player\n");
    strcat(body, "/unfreeze <id> - unfreeze a player\n");
    strcat(body, "/goto <id> - teleport to a player\n");
    strcat(body, "/gethere <id> - bring a player to you\n");
    strcat(body, "/save - force-save your account\n");
    strcat(body, "\n{FFCC33}Admin (2+){FFFFFF}\n");
    strcat(body, "/givecash <id> <amt> - give money\n");
    strcat(body, "/setskin <id> <skin> - set a skin\n");
    strcat(body, "\n{FFCC33}Manager (3+){FFFFFF}\n");
    strcat(body, "/setlevel <id> <0-4> - set admin level\n");
    strcat(body, "/setvip <id> <0/1> - toggle VIP\n");
    new title[48];
    format(title, sizeof title, "Admin commands - %s", LevelName(g_acc[playerid][Acc_Level]));
    ShowPlayerDialog(playerid, DLG_ADMINCMDS, DIALOG_STYLE_MSGBOX, title, body, "Close", "");
    return 1;
}

Cmd_Kick(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_MOD)) return 1;
    new t = FindTarget(params);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Usage: /kick <id/name>"); return 1; }
    new m[128]; format(m, sizeof m, "%s kicked by %s.", GetPlayerNameEx(t), GetPlayerNameEx(playerid));
    SendClientMessageToAll(COL_ADMIN, m); Kick(t);
    return 1;
}

Cmd_Ban(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_ADMIN)) return 1;
    new t = FindTarget(params);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Usage: /ban <id/name>"); return 1; }
    g_acc[t][Acc_Banned] = true; SavePlayer(t);
    new m[128]; format(m, sizeof m, "%s banned by %s.", GetPlayerNameEx(t), GetPlayerNameEx(playerid));
    SendClientMessageToAll(COL_ADMIN, m); Kick(t);
    return 1;
}

Cmd_Unban(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_ADMIN)) return 1;
    if (!params[0]) { SendClientMessage(playerid, COL_ERR, "Usage: /unban <name>"); return 1; }
    new q[160];
    mysql_format(g_db, q, sizeof q, "UPDATE accounts SET banned=0 WHERE name='%e'", params);
    mysql_execute(g_db, q);
    new m[96]; format(m, sizeof m, "Account '%s' unbanned.", params);
    SendClientMessage(playerid, COL_OK, m);
    return 1;
}

Cmd_Mute(playerid, const params[], bool:on)
{
    if (!RequireLevel(playerid, LEVEL_MOD)) return 1;
    new t = FindTarget(params);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Usage: /(un)mute <id/name>"); return 1; }
    g_acc[t][Acc_Muted] = on;
    SendClientMessage(t, on ? COL_ERR : COL_OK, on ? "You were muted." : "You were unmuted.");
    SendClientMessage(playerid, COL_OK, on ? "Player muted." : "Player unmuted.");
    return 1;
}

Cmd_Freeze(playerid, const params[], bool:on)
{
    if (!RequireLevel(playerid, LEVEL_MOD)) return 1;
    new t = FindTarget(params);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Usage: /(un)freeze <id/name>"); return 1; }
    TogglePlayerControllable(t, !on); g_acc[t][Acc_Frozen] = on;
    SendClientMessage(t, on ? COL_ERR : COL_OK, on ? "You were frozen." : "You were unfrozen.");
    SendClientMessage(playerid, COL_OK, on ? "Player frozen." : "Player unfrozen.");
    return 1;
}

Cmd_Goto(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_MOD)) return 1;
    new t = FindTarget(params);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Usage: /goto <id/name>"); return 1; }
    new Float:x, Float:y, Float:z; GetPlayerPos(t, x, y, z);
    SetPlayerInterior(playerid, GetPlayerInterior(t)); SetPlayerVirtualWorld(playerid, GetPlayerVirtualWorld(t));
    SetPlayerPos(playerid, x + 1.5, y, z);
    SendClientMessage(playerid, COL_OK, "Teleported.");
    return 1;
}

Cmd_GetHere(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_MOD)) return 1;
    new t = FindTarget(params);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Usage: /gethere <id/name>"); return 1; }
    new Float:x, Float:y, Float:z; GetPlayerPos(playerid, x, y, z);
    SetPlayerInterior(t, GetPlayerInterior(playerid)); SetPlayerVirtualWorld(t, GetPlayerVirtualWorld(playerid));
    SetPlayerPos(t, x + 1.5, y, z);
    SendClientMessage(playerid, COL_OK, "Player teleported to you.");
    return 1;
}

Cmd_GiveCash(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_ADMIN)) return 1;
    new tStr[32], aStr[16], i = 0;
    Tok(params, i, tStr, sizeof tStr); Tok(params, i, aStr, sizeof aStr);
    if (!tStr[0] || !aStr[0]) { SendClientMessage(playerid, COL_ERR, "Usage: /givecash <id/name> <amount>"); return 1; }
    new t = FindTarget(tStr);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Player not found."); return 1; }
    new amt = strval(aStr);
    GivePlayerMoney(t, amt);
    new m[96]; format(m, sizeof m, "Gave $%d to %s.", amt, GetPlayerNameEx(t));
    SendClientMessage(playerid, COL_OK, m);
    return 1;
}

Cmd_SetSkin(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_ADMIN)) return 1;
    new tStr[32], sStr[8], i = 0;
    Tok(params, i, tStr, sizeof tStr); Tok(params, i, sStr, sizeof sStr);
    if (!tStr[0] || !sStr[0]) { SendClientMessage(playerid, COL_ERR, "Usage: /setskin <id/name> <skin>"); return 1; }
    new t = FindTarget(tStr);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Player not found."); return 1; }
    new skin = strval(sStr);
    SetPlayerSkin(t, skin);
    SendClientMessage(playerid, COL_OK, "Skin set.");
    return 1;
}

Cmd_SetLevel(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_MANAGER)) return 1;
    new tStr[32], lStr[8], i = 0;
    Tok(params, i, tStr, sizeof tStr); Tok(params, i, lStr, sizeof lStr);
    if (!tStr[0] || !lStr[0]) { SendClientMessage(playerid, COL_ERR, "Usage: /setlevel <id/name> <0-4>"); return 1; }
    new t = FindTarget(tStr);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Player not found."); return 1; }
    new level = strval(lStr);
    if (level < LEVEL_PLAYER || level > LEVEL_OWNER) { SendClientMessage(playerid, COL_ERR, "Level 0-4."); return 1; }
    g_acc[t][Acc_Level] = level; SavePlayer(t);
    new m[96]; format(m, sizeof m, "%s is now %s.", GetPlayerNameEx(t), LevelName(level));
    SendClientMessage(playerid, COL_OK, m); SendClientMessage(t, COL_ADMIN, m);
    return 1;
}

Cmd_SetVip(playerid, const params[])
{
    if (!RequireLevel(playerid, LEVEL_MANAGER)) return 1;
    new tStr[32], vStr[8], i = 0;
    Tok(params, i, tStr, sizeof tStr); Tok(params, i, vStr, sizeof vStr);
    if (!tStr[0] || !vStr[0]) { SendClientMessage(playerid, COL_ERR, "Usage: /setvip <id/name> <0/1>"); return 1; }
    new t = FindTarget(tStr);
    if (t == INVALID_PLAYER_ID) { SendClientMessage(playerid, COL_ERR, "Player not found."); return 1; }
    g_acc[t][Acc_Vip] = (strval(vStr) != 0); SavePlayer(t);
    new m[96]; format(m, sizeof m, "%s VIP %s.", GetPlayerNameEx(t), g_acc[t][Acc_Vip] ? "ON" : "OFF");
    SendClientMessage(playerid, COL_OK, m);
    return 1;
}

public OnPlayerText(playerid, text[])
{
    if (g_acc[playerid][Acc_Muted]) { SendClientMessage(playerid, COL_ERR, "You are muted."); return 0; }
    return 1;
}

public OnPlayerDeath(playerid, killerid, WEAPON:reason)
{
    #pragma unused reason
    if (g_acc[playerid][Acc_Logged]) g_acc[playerid][Acc_Deaths]++;
    if (killerid != INVALID_PLAYER_ID && g_acc[killerid][Acc_Logged]) g_acc[killerid][Acc_Kills]++;
    return 1;
}

stock GetPlayerNameEx(playerid)
{
    new n[MAX_PLAYER_NAME];
    GetPlayerName(playerid, n, sizeof n);
    return n;
}
