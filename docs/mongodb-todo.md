# MongoDB API Layer TODO (MimicAPI)

This file tracks the remaining Mongo-style API surface to implement on top of MimicAPI primitives.
All logic here stays in the API layer; MimicDB remains a dumb execution engine.

## Core Queries
- [ ] `find()` additional filter operators: `$not`, `$nor`, `$all`, `$size`
- [ ] `find()` projection features: computed fields, `$slice`, exclusion rules parity
- [ ] `find()` cursor helpers: `sort`, `skip`, `limit` as chainable calls
- [ ] `find()` support for nested field paths (dot notation)
- [ ] `find()` support for array field matching semantics

## Updates
- [ ] `update_one()`/`update_many()` with `upsert`
- [ ] Update operators: `$setOnInsert`, `$rename`, `$currentDate`
- [ ] Array update operators: `$push`, `$pull`, `$addToSet`
- [ ] `$unset` with multi-field shorthand
- [ ] `replace_one()` options parity

## Deletes
- [ ] `delete_one()`/`delete_many()` options parity
- [ ] `find_one_and_delete()` / `find_one_and_update()` helpers

## Aggregations
- [ ] `$group` multi-field + `$first`/`$last`
- [ ] `$count`, `$sortByCount`
- [ ] `$addFields` / `$set`
- [ ] `$unwind`
- [ ] `$lookup` (API-only joins)
- [ ] `$facet` (multi-pipeline)
- [ ] `$project` computed expressions
- [ ] `$match` with full operator support

## Index & Hints
- [ ] `create_index()` and `drop_index()` stubs (no backend effect)
- [ ] Accept and ignore `hint` for compatibility

## Bulk Operations
- [ ] `bulk_write()` minimal support
- [ ] `insert_one()`/`insert_many()` options parity

## Sessions & Transactions (API-only)
- [ ] Session tokens (API-local)
- [ ] `with_transaction()` stub for compatibility

## Error & Result Parity
- [ ] Mongo-style error classes
- [ ] Result objects parity (`acknowledged`, `matched_count`, `modified_count`)
- [ ] Duplicate key error mapping for `_id`

## Tests
- [ ] Operator coverage tests (`$in/$nin/$exists/$regex/$not/$nor/$all/$size`)
- [ ] Update operator tests
- [ ] Aggregation pipeline tests
- [ ] Compatibility tests for common Mongo workflows
