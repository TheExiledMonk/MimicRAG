import unittest

from mimicapi import MongoClient


class TestMongoDBLayer(unittest.TestCase):
    def test_insert_many_and_aggregate(self) -> None:
        client = MongoClient()
        users = client["default"]["users"]
        result = users.insert_many(
            [
                {"age": 20, "income": 100.0},
                {"age": 40, "income": 250.0},
                {"age": 35, "income": 300.0},
            ]
        )
        self.assertEqual(result.inserted_count, 3)

        pipeline = [
            {"$match": {"age": {"$gt": 30}}},
            {"$group": {"_id": None, "count": {"$sum": 1}, "sum_income": {"$sum": "$income"}}},
        ]
        out = users.aggregate(pipeline)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["count"], 2)
        self.assertEqual(out[0]["sum_income"], 550.0)

    def test_find_update_delete(self) -> None:
        client = MongoClient()
        items = client["default"]["items"]
        items.insert_many(
            [
                {"sku": 1, "price": 10.0, "tag": None},
                {"sku": 2, "price": 20.0, "tag": 1},
                {"sku": 3, "price": 30.0},
            ]
        )

        found = items.find({"price": {"$gte": 20}}, projection=["sku"]).to_list()
        self.assertEqual(len(found), 2)
        self.assertTrue(all("price" not in doc for doc in found))

        in_filter = items.find({"sku": {"$in": [1, 3]}}).to_list()
        self.assertEqual({doc["sku"] for doc in in_filter}, {1, 3})

        exists = items.find({"tag": {"$exists": True}}).to_list()
        self.assertEqual(len(exists), 1)
        self.assertEqual(exists[0]["sku"], 2)

        update = items.update_one({"sku": 2}, {"$set": {"price": 22.0}})
        self.assertEqual(update.matched_count, 1)
        self.assertEqual(update.modified_count, 1)
        self.assertEqual(items.find_one({"sku": 2})["price"], 22.0)

        deleted = items.delete_many({"sku": {"$lte": 2}})
        self.assertEqual(deleted.deleted_count, 2)
        remaining = items.find().to_list()
        self.assertEqual(len(remaining), 1)
        self.assertEqual(remaining[0]["sku"], 3)

    def test_group_by_fields(self) -> None:
        client = MongoClient()
        sales = client["default"]["sales"]
        sales.insert_many(
            [
                {"country": 1, "year": 2023, "revenue": 10.0},
                {"country": 1, "year": 2024, "revenue": 20.0},
                {"country": 2, "year": 2024, "revenue": 15.0},
            ]
        )
        pipeline = [
            {"$group": {"_id": {"country": "$country"}, "total": {"$sum": "$revenue"}}},
            {"$sort": {"total": -1}},
        ]
        out = sales.aggregate(pipeline)
        self.assertEqual(out[0]["_id"]["country"], 1)
        self.assertEqual(out[0]["total"], 30.0)

    def test_regex_match(self) -> None:
        client = MongoClient()
        names = client["default"]["names"]
        names.insert_many(
            [
                {"name": "alice"},
                {"name": "bob"},
                {"name": "albert"},
            ]
        )
        results = names.find({"name": {"$regex": "^al"}}).to_list()
        self.assertEqual(len(results), 2)

    def test_array_and_dot_filters(self) -> None:
        client = MongoClient()
        docs = client["default"]["docs"]
        docs.insert_many(
            [
                {"tags": [1, 2], "meta": {"score": 5}, "items": [{"k": 1}, {"k": 2}]},
                {"tags": [3], "meta": {"score": 2}, "items": [{"k": 3}]},
            ]
        )
        tag_match = docs.find({"tags": 2}).to_list()
        self.assertEqual(len(tag_match), 1)
        all_match = docs.find({"tags": {"$all": [1, 2]}}).to_list()
        self.assertEqual(len(all_match), 1)
        size_match = docs.find({"tags": {"$size": 1}}).to_list()
        self.assertEqual(len(size_match), 1)
        dot_match = docs.find({"meta.score": {"$gt": 3}}).to_list()
        self.assertEqual(len(dot_match), 1)
        nested_match = docs.find({"items.k": {"$in": [2]}}).to_list()
        self.assertEqual(len(nested_match), 1)
        nor_match = docs.find({"$nor": [{"meta.score": {"$gt": 3}}]}).to_list()
        self.assertEqual(len(nor_match), 1)
        not_match = docs.find({"meta.score": {"$not": {"$gt": 3}}}).to_list()
        self.assertEqual(len(not_match), 1)

    def test_filter_operator_coverage(self) -> None:
        client = MongoClient()
        docs = client["default"]["filters"]
        docs.insert_many(
            [
                {"name": "alpha", "score": 1, "tags": [1, 2]},
                {"name": "beta", "score": 2, "tags": [2]},
                {"name": "gamma", "score": 3, "tags": []},
            ]
        )
        in_match = docs.find({"score": {"$in": [2, 3]}}).to_list()
        self.assertEqual({doc["name"] for doc in in_match}, {"beta", "gamma"})
        nin_match = docs.find({"score": {"$nin": [1]}}).to_list()
        self.assertEqual({doc["name"] for doc in nin_match}, {"beta", "gamma"})
        exists_true = docs.find({"tags": {"$exists": True}}).to_list()
        self.assertEqual(len(exists_true), 3)
        exists_false = docs.find({"missing": {"$exists": False}}).to_list()
        self.assertEqual(len(exists_false), 3)
        regex_match = docs.find({"name": {"$regex": "a$"}}).to_list()
        self.assertEqual({doc["name"] for doc in regex_match}, {"alpha", "gamma"})
        not_match = docs.find({"score": {"$not": {"$gt": 1}}}).to_list()
        self.assertEqual({doc["name"] for doc in not_match}, {"alpha"})
        nor_match = docs.find({"$nor": [{"score": {"$gt": 2}}]}).to_list()
        self.assertEqual({doc["name"] for doc in nor_match}, {"alpha", "beta"})
        all_match = docs.find({"tags": {"$all": [1, 2]}}).to_list()
        self.assertEqual(len(all_match), 1)
        size_match = docs.find({"tags": {"$size": 1}}).to_list()
        self.assertEqual(len(size_match), 1)

    def test_projection_slice_and_literal(self) -> None:
        client = MongoClient()
        items = client["default"]["proj"]
        items.insert_many(
            [
                {"name": "alpha", "tags": [1, 2, 3]},
            ]
        )
        projected = items.find(
            projection={
                "tags": {"$slice": [1, 1]},
                "name_copy": {"$field": "name"},
                "constant": {"$literal": 7},
            }
        ).to_list()
        self.assertEqual(projected[0]["tags"], [2])
        self.assertEqual(projected[0]["name_copy"], "alpha")
        self.assertEqual(projected[0]["constant"], 7)

    def test_update_ops_and_upsert(self) -> None:
        client = MongoClient()
        items = client["default"]["ops"]
        items.insert_many(
            [
                {"sku": 1, "tags": [1, 2], "meta": {"count": 1}, "updated_at": None},
                {"sku": 2, "tags": [2], "meta": {"count": 2}, "updated_at": None},
            ]
        )

        items.update_one({"sku": 1}, {"$inc": {"meta.count": 2}})
        doc = items.find_one({"sku": 1})
        self.assertEqual(doc["meta"]["count"], 3)

        items.update_one({"sku": 1}, {"$push": {"tags": 3}})
        items.update_one({"sku": 1}, {"$addToSet": {"tags": 3}})
        items.update_one({"sku": 1}, {"$pull": {"tags": {"$in": [1, 3]}}})
        doc = items.find_one({"sku": 1})
        self.assertEqual(doc["tags"], [2])

        items.update_one({"sku": 2}, {"$rename": {"meta.count": "meta.total"}})
        items.update_one({"sku": 2}, {"$currentDate": {"updated_at": True}})
        doc = items.find_one({"sku": 2})
        self.assertNotIn("count", doc["meta"])
        self.assertEqual(doc["meta"]["total"], 2)
        self.assertIsInstance(doc["updated_at"], float)

        upsert_result = items.update_one(
            {"sku": 5},
            {"$set": {"tags": [9]}, "$setOnInsert": {"meta": {"count": 1}, "updated_at": 0.0}},
            upsert=True,
        )
        self.assertEqual(upsert_result.matched_count, 0)
        self.assertEqual(upsert_result.modified_count, 1)
        doc = items.find_one({"sku": 5})
        self.assertEqual(doc["tags"], [9])
        self.assertEqual(doc["meta"]["count"], 1)
        self.assertEqual(doc["updated_at"], 0.0)

    def test_update_operator_coverage(self) -> None:
        client = MongoClient()
        items = client["default"]["update_ops"]
        items.insert_many(
            [
                {"sku": 1, "price": 10.0, "meta": {"count": 1}, "tags": [1]},
            ]
        )
        items.update_one({"sku": 1}, {"$mul": {"price": 2}})
        items.update_one({"sku": 1}, {"$min": {"price": 25.0}})
        items.update_one({"sku": 1}, {"$max": {"price": 19.0}})
        items.update_one({"sku": 1}, {"$rename": {"meta.count": "meta.total"}})
        items.update_one({"sku": 1}, {"$unset": ["meta.total"]})
        items.update_one({"sku": 1}, {"$addToSet": {"tags": {"$each": [1, 2]}}})
        doc = items.find_one({"sku": 1})
        self.assertEqual(doc["price"], 20.0)
        self.assertNotIn("total", doc["meta"])
        self.assertEqual(doc["tags"], [1, 2])

    def test_find_one_and_delete_update(self) -> None:
        client = MongoClient()
        items = client["default"]["find_one"]
        items.insert_many(
            [
                {"sku": 1, "score": 10},
                {"sku": 2, "score": 5},
            ]
        )
        deleted = items.find_one_and_delete({"score": {"$gte": 10}}, projection=["sku"])
        self.assertEqual(deleted["sku"], 1)
        remaining = items.find().to_list()
        self.assertEqual(len(remaining), 1)

        updated = items.find_one_and_update(
            {"sku": 2},
            {"$inc": {"score": 3}},
            return_document="after",
        )
        self.assertEqual(updated["score"], 8)
        before = items.find_one_and_update(
            {"sku": 2},
            {"$inc": {"score": 2}},
            return_document="before",
        )
        self.assertEqual(before["score"], 8)

    def test_compatibility_workflow(self) -> None:
        client = MongoClient()
        users = client["default"]["workflow"]
        users.insert_many(
            [
                {"name": "alice", "age": 30, "team": "a"},
                {"name": "bob", "age": 22, "team": "a"},
                {"name": "cora", "age": 41, "team": "b"},
            ]
        )
        users.update_many({"team": "a"}, {"$inc": {"age": 1}})
        results = users.find({"age": {"$gte": 31}}, projection=["name"]).to_list()
        self.assertEqual({doc["name"] for doc in results}, {"alice", "cora"})
        pipeline = [
            {"$match": {"team": "a"}},
            {"$group": {"_id": "$team", "count": {"$sum": 1}}},
        ]
        out = users.aggregate(pipeline)
        self.assertEqual(out[0]["count"], 2)
        deleted = users.find_one_and_delete({"name": "bob"})
        self.assertEqual(deleted["name"], "bob")
        remaining = users.count_documents({})
        self.assertEqual(remaining, 2)

    def test_aggregate_stages(self) -> None:
        client = MongoClient()
        items = client["default"]["agg"]
        items.insert_many(
            [
                {"sku": 1, "category": "a", "price": 10.0, "tags": [1, 2]},
                {"sku": 2, "category": "a", "price": 15.0, "tags": [2]},
                {"sku": 3, "category": "b", "price": 7.0, "tags": []},
            ]
        )
        out = items.aggregate(
            [
                {"$group": {"_id": {"cat": "$category"}, "first_price": {"$first": "$price"}, "last_price": {"$last": "$price"}}},
                {"$sort": {"first_price": -1}},
            ]
        )
        self.assertEqual(out[0]["_id"]["cat"], "a")
        self.assertEqual(out[0]["first_price"], 10.0)
        self.assertEqual(out[0]["last_price"], 15.0)

        counted = items.aggregate([{"$count": "total"}])
        self.assertEqual(counted[0]["total"], 3)
        by_count = items.aggregate([{"$sortByCount": "$category"}])
        self.assertEqual(by_count[0]["_id"], "a")

        added = items.aggregate([{"$addFields": {"price_copy": "$price"}}, {"$project": {"price_copy": 1}}])
        self.assertEqual(added[0]["price_copy"], 10.0)

        unwound = items.aggregate([{"$unwind": "$tags"}])
        self.assertEqual(len(unwound), 3)

        lookup = items.aggregate(
            [
                {"$lookup": {"from": "agg", "localField": "category", "foreignField": "category", "as": "same_cat"}},
                {"$project": {"same_cat": 1}},
            ]
        )
        self.assertTrue(all(len(doc["same_cat"]) >= 1 for doc in lookup))

        facet = items.aggregate(
            [
                {"$facet": {"counts": [{"$count": "total"}], "cats": [{"$sortByCount": "$category"}]}},
            ]
        )
        self.assertEqual(facet[0]["counts"][0]["total"], 3)
        self.assertEqual(facet[0]["cats"][0]["_id"], "a")

    def test_bulk_write(self) -> None:
        client = MongoClient()
        items = client["default"]["bulk"]
        result = items.bulk_write(
            [
                {"insert_one": {"sku": 1, "price": 10.0}},
                {"insert_one": {"sku": 2, "price": 12.0}},
                {"update_one": {"filter": {"sku": 2}, "update": {"$inc": {"price": 3.0}}}},
                {"delete_one": {"filter": {"sku": 1}}},
            ]
        )
        self.assertEqual(result.inserted_count, 2)
        self.assertEqual(result.modified_count, 1)
        self.assertEqual(result.deleted_count, 1)
        remaining = items.find().to_list()
        self.assertEqual(len(remaining), 1)
        self.assertEqual(remaining[0]["sku"], 2)


if __name__ == "__main__":
    unittest.main()
