# Modern MySQL 8 Features Worth Adding to a SA:MP MySQL Plugin

## Overview

This document focuses on features introduced or significantly improved in modern MySQL 8.x that are often missing from older-era SA:MP MySQL plugins and gamemodes.

---

## High-Priority Features

### 1. Prepared Statements

**Benefits**
- SQL injection protection
- Better performance for repeated queries
- Reduced query parsing overhead

**Suggested API**

```pawn
mysql_prepare(...)
mysql_bind_int(...)
mysql_bind_string(...)
mysql_execute(...)
```

---

### 2. UTF8MB4 Support

**Benefits**
- Full Unicode support
- Emoji support
- Better internationalisation

**Requirements**
- UTF8MB4 connection support
- UTF8MB4 query support
- UTF8MB4 result handling

---

### 3. Modern Authentication

Support modern MySQL authentication methods:

- `caching_sha2_password`
- `mysql_native_password`

This resolves compatibility issues with newer MySQL installations.

---

### 4. Connection Pooling

Instead of opening and closing connections repeatedly:

```text
Pool
├─ Connection 1
├─ Connection 2
├─ Connection 3
└─ Connection N
```

**Benefits**
- Lower latency
- Better throughput
- Improved scalability

---

### 5. Enhanced Transactions

Suggested API:

```pawn
mysql_begin_transaction(...)
mysql_commit(...)
mysql_rollback(...)
```

Useful for:

- Banking systems
- Vehicle purchases
- Property purchases
- Marketplace transactions

---

## SQL Features in MySQL 8

### 6. Window Functions

Example:

```sql
SELECT
    name,
    money,
    RANK() OVER (
        ORDER BY money DESC
    ) AS rank
FROM players;
```

**Use Cases**
- Wealth leaderboards
- Kill rankings
- Event rankings
- Faction rankings

---

### 7. Common Table Expressions (CTEs)

Example:

```sql
WITH rich_players AS (
    SELECT *
    FROM players
    WHERE money > 1000000
)
SELECT *
FROM rich_players;
```

**Benefits**
- Cleaner complex queries
- Easier maintenance

---

### 8. Recursive CTEs

Useful for:

- Faction hierarchies
- Admin hierarchies
- Organisational structures

---

### 9. JSON Data Type

Example:

```sql
inventory JSON
```

```json
{
  "weapon_24": 150,
  "weapon_31": 500
}
```

**Benefits**
- Flexible inventories
- Dynamic player metadata
- Reduced schema complexity

---

### 10. JSON Functions

Example:

```sql
SELECT *
FROM players
WHERE JSON_EXTRACT(
    inventory,
    '$.weapon_24'
) > 100;
```

---

### 11. JSON Aggregation

Example:

```sql
SELECT JSON_ARRAYAGG(name)
FROM players;
```

Useful for:

- APIs
- UCP systems
- Discord integrations

---

### 12. Generated Columns

Example:

```sql
kdr AS (kills / deaths)
```

**Benefits**
- Less Pawn code
- Automatically calculated values

---

### 13. Functional Indexes

Example:

```sql
CREATE INDEX idx_lower_name
ON players ((LOWER(name)));
```

**Benefits**
- Faster searches
- Better performance for case-insensitive lookups

---

### 14. CHECK Constraints

Example:

```sql
ALTER TABLE players
ADD CONSTRAINT chk_money
CHECK (money >= 0);
```

**Benefits**
- Better data integrity
- Reduced corruption

---

### 15. Invisible Indexes

Useful for:
- Performance testing
- Query optimisation

---

### 16. Descending Indexes

Useful for:
- Leaderboards
- Ranking queries
- Ordered reports

---

### 17. Histograms

Useful for:
- Better query planning
- Faster execution of complex queries

---

## Medium-Priority Plugin Enhancements

### Bulk Inserts

Suggested API:

```pawn
mysql_bulk_insert(...)
```

Useful for:
- Logging systems
- Batch processing

---

### JSON Helpers

Suggested API:

```pawn
mysql_json_get(...)
mysql_json_set(...)
```

---

### Query Profiling

Suggested API:

```pawn
mysql_profile_query(...)
```

Useful for:
- Performance analysis
- Debugging

---

## Advanced Features

### Async Batch Processing

Suggested API:

```pawn
mysql_batch(...)
```

---

### Promise/Future Style Callbacks

Modern asynchronous query handling.

---

### Built-In Migration Framework

Suggested API:

```pawn
mysql_migrate(...)
```

Useful for:
- Schema upgrades
- Versioned deployments

---

## Recommended Priority Order

### Phase 1
1. UTF8MB4 Support
2. Modern Authentication
3. Prepared Statements
4. Connection Pooling
5. Improved Transactions

### Phase 2
1. JSON Helpers
2. Bulk Inserts
3. Query Profiling

### Phase 3
1. Async Batch Processing
2. Migration Framework
3. Promise/Future APIs

## Summary

For most existing SA:MP servers, the highest-value improvements are:

- Prepared Statements
- UTF8MB4 Support
- Modern Authentication
- Connection Pooling
- Enhanced Transaction APIs
- JSON Support

These provide the greatest practical benefit while maintaining compatibility with existing gamemodes.
