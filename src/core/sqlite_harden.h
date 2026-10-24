/*
 * Liu — opening a SQLite file somebody else wrote.
 *
 * The agent-history scanners read OpenCode's opencode.db and the VS Code
 * `state.vscdb` family (Cursor, Antigravity, code-oss); the bridge reads and
 * writes OpenCode's too. Those files are not Liu's, and a database is more than
 * rows: its SCHEMA is SQL that runs when the database is used. A view can stand
 * in for a table the scanner SELECTs from, a trigger can fire on a write, and a
 * CHECK or DEFAULT expression can call functions — so "open it read-only and
 * run a fixed query" still executes whatever SQL the file's author put in
 * sqlite_master. That matters most when the engine itself has a bug a crafted
 * statement reaches (before SQLite 3.50.2, an aggregate with more terms than
 * there are columns corrupts the heap, and a schema view can reach it).
 *
 * These switches take the schema out of the picture:
 *   DEFENSIVE       no writes to shadow tables, no writable_schema;
 *   TRUSTED_SCHEMA  off: functions called from schema objects must be innocuous;
 *   TRIGGER, VIEW   off: neither runs. OpenCode's database and the
 *                   state.vscdb files have no view or trigger, so the fixed
 *                   queries are unaffected;
 *   LIMIT_LENGTH    a string or blob over 128 MB is an error, not an allocation
 *                   (the scanners cap what they keep far below that);
 *   EXPR_DEPTH      hostile nesting stops at 200, not 1000.
 *
 * Apply right after every sqlite3_open of a file somebody else could have written:
 * other tools' databases, and the vault's own (src/vault/vault.c) — its header models
 * a DB-write adversary, and it too uses only plain tables, indexes and ADD COLUMN.
 * Pure configuration: safe on a NULL handle.
 */
#ifndef CORE_SQLITE_HARDEN_H
#define CORE_SQLITE_HARDEN_H

#include "sqlite3.h"

static inline void liu_sqlite_harden_foreign(sqlite3 *db) {
    if (!db) return;
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, (int *)0);
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, (int *)0);
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, (int *)0);
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, (int *)0);
    (void)sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 128 * 1024 * 1024);
    (void)sqlite3_limit(db, SQLITE_LIMIT_EXPR_DEPTH, 200);
}

#endif /* CORE_SQLITE_HARDEN_H */
