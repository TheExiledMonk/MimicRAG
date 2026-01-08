# Mongo C++ Core Parity TODO

Goal: bring `MongoClientCpp`/C++ core up to parity with the current Python Mongo API.

## Query + Filters
- [ ] `$nin` operator
- [ ] `$not` operator
- [ ] `$nor` operator
- [ ] `$all` operator
- [ ] `$size` operator
- [ ] `$regex` operator + `$options` flags
- [ ] Nested field paths (dot notation)
- [ ] Array field matching semantics
- [ ] `$exists` semantics parity (null vs missing)

## Projection
- [x] Inclusion/exclusion parity
- [x] Computed fields (`$literal`, `$field`)
- [x] `$slice` for arrays

## Cursor Helpers
- [x] `sort` / `skip` / `limit` support
- [x] Chainable cursor behavior

## Aggregation Pipeline
- [x] `$match` full operator support
- [x] `$group` multi-field `_id`
- [x] `$first` / `$last`
- [x] `$count` and `$sortByCount`
- [x] `$addFields` / `$set`
- [x] `$project` computed expressions
- [x] `$unwind`
- [x] `$lookup` (API-only join)
- [x] `$facet` (multi-pipeline)

## Updates
- [x] `update_one` / `update_many` upsert
- [x] `$setOnInsert`, `$rename`, `$currentDate`
- [x] `$unset` shorthand (list)
- [x] Array updates: `$push`, `$pull`, `$addToSet`
- [x] `replace_one` options parity

## Deletes
- [x] `delete_one` / `delete_many` options parity
- [x] `find_one_and_delete`

## Find One And Update
- [x] `find_one_and_update` parity (projection/sort/return_document/upsert)

## Bulk Operations
- [x] `bulk_write` minimal support
- [x] `insert_one`/`insert_many` options parity

## Index & Hints
- [x] `create_index` / `drop_index` stubs
- [x] Accept/ignore `hint`

## Sessions/Transactions (API-only)
- [x] Session token support
- [x] `with_transaction()` stub

## Errors & Results
- [x] Mongo-style errors and duplicate key mapping
- [x] Result object parity (`acknowledged`, counts)

## Tests
- [x] Operator coverage tests (`$in/$nin/$exists/$regex/$not/$nor/$all/$size`)
- [x] Update operator tests
- [x] Aggregation pipeline tests
- [x] Compatibility workflow tests
