import unittest

from mimicapi import MongoClientCpp


class TestMongoCppAPI(unittest.TestCase):
    def test_find_filters_update_delete(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "age": 10},
            {"name": "beta", "age": 20},
            {"name": "gamma", "age": 30},
        ]
        client.insert_many("default", "users", docs)
        results = client.find("default", "users", {"age": {"$gt": 15}}).to_list()
        self.assertEqual({doc["name"] for doc in results}, {"beta", "gamma"})

        results = client.find("default", "users", {"name": {"$in": ["alpha", "gamma"]}}).to_list()
        self.assertEqual({doc["name"] for doc in results}, {"alpha", "gamma"})

        matched = client.update(
            "default",
            "users",
            {"name": "beta"},
            {"$set": {"age": 25}},
        )
        self.assertEqual(matched, 1)
        updated = client.find("default", "users", {"age": {"$eq": 25}}).to_list()
        self.assertEqual(len(updated), 1)

        deleted = client.delete("default", "users", {"name": "alpha"})
        self.assertEqual(deleted, 1)
        remaining = client.find("default", "users", {}).to_list()
        self.assertEqual({doc["name"] for doc in remaining}, {"beta", "gamma"})

    def test_aggregate_group(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"country": 1, "income": 10.0},
            {"country": 1, "income": 20.0},
            {"country": 2, "income": 15.0},
        ]
        client.insert_many("default", "sales", docs)
        out = client.aggregate(
            "default",
            "sales",
            [
                {"$group": {"_id": "$country", "sum": {"$sum": "$income"}, "count": {"$sum": 1}}},
            ],
        )
        results = {doc["_id"]: doc["sum"] for doc in out}
        self.assertEqual(results[1], 30.0)

    def test_filter_extras(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "age": 10},
            {"name": "beta", "age": 20},
            {"name": None, "age": 30},
        ]
        client.insert_many("default", "extra", docs)
        results = client.find("default", "extra", {"age": {"$nin": [10]}}).to_list()
        self.assertEqual({doc["age"] for doc in results}, {20, 30})
        results = client.find("default", "extra", {"name": {"$exists": False}}).to_list()
        self.assertEqual(len(results), 0)
        results = client.find("default", "extra", {"name": {"$exists": True}}).to_list()
        self.assertEqual(len(results), 2)
        results = client.find("default", "extra", {"name": {"$regex": "^a"}}).to_list()
        self.assertEqual(len(results), 1)
        results = client.find("default", "extra", {"age": {"$not": {"$gt": 10}}}).to_list()
        self.assertEqual({doc["age"] for doc in results}, {10})
        results = client.find(
            "default",
            "extra",
            {"$nor": [{"age": {"$gt": 15}}, {"name": {"$exists": False}}]},
        ).to_list()
        self.assertEqual({doc["age"] for doc in results}, {10})
        results = client.find(
            "default",
            "extra",
            {"name": {"$in": ["alpha", "beta"]}},
        ).to_list()
        self.assertEqual({doc["name"] for doc in results}, {"alpha", "beta"})
        extra_docs = [
            {"name": "delta", "tags": [1, 2, 3]},
            {"name": "epsilon", "tags": [2, 3]},
        ]
        client.insert_many("default", "extra", extra_docs)
        results = client.find("default", "extra", {"tags": {"$all": [2, 3]}}).to_list()
        self.assertEqual({doc["name"] for doc in results}, {"delta", "epsilon"})
        results = client.find("default", "extra", {"tags": {"$size": 3}}).to_list()
        self.assertEqual({doc["name"] for doc in results}, {"delta"})

    def test_array_slice(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "tags": [1, 2, 3, 4]},
            {"name": "beta", "tags": ["a", "b", "c"]},
        ]
        client.insert_many("default", "arrays", docs)
        out = client.find(
            "default",
            "arrays",
            {"name": "alpha"},
            projection={"tags": {"$slice": 2}},
        ).to_list()
        self.assertEqual(out[0]["tags"], [1, 2])
        out = client.find(
            "default",
            "arrays",
            {"name": "beta"},
            projection={"tags": {"$slice": [-1, 1]}},
        ).to_list()
        self.assertEqual(out[0]["tags"], ["c"])

    def test_cursor_sort_skip_limit(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "score": 10},
            {"name": "beta", "score": 5},
            {"name": "gamma", "score": 15},
        ]
        client.insert_many("default", "cursor", docs)
        cursor = client.find("default", "cursor", {})
        results = cursor.sort([("score", -1)]).skip(1).limit(1).to_list()
        self.assertEqual(results[0]["name"], "alpha")

    def test_aggregate_match_group_count(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"team": "red", "user": "a", "score": 10},
            {"team": "red", "user": "b", "score": 12},
            {"team": "blue", "user": "a", "score": 9},
            {"team": "blue", "user": "c", "score": 11},
        ]
        client.insert_many("default", "agg", docs)
        out = client.aggregate(
            "default",
            "agg",
            [
                {"$match": {"$or": [{"team": "red"}, {"score": {"$gt": 10}}]}},
                {"$group": {
                    "_id": {"team": "$team", "user": "$user"},
                    "first": {"$first": "$score"},
                    "last": {"$last": "$score"},
                    "count": {"$sum": 1},
                }},
                {"$sortByCount": "$_id"},
            ],
        )
        self.assertTrue(out)
        count_stage = client.aggregate(
            "default",
            "agg",
            [
                {"$match": {"team": {"$in": ["red", "blue"]}}},
                {"$count": "total"},
            ],
        )
        self.assertEqual(count_stage[0]["total"], 4)
        match_all = client.aggregate(
            "default",
            "agg",
            [
                {"$match": {"$nor": [{"team": "green"}]}},
                {"$group": {"_id": "$team", "count": {"$sum": 1}}},
            ],
        )
        self.assertEqual(sum(doc["count"] for doc in match_all), 4)

    def test_aggregate_add_fields_project_unwind(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "scores": [1, 2]},
            {"name": "beta", "scores": [3]},
        ]
        client.insert_many("default", "pipeline", docs)
        out = client.aggregate(
            "default",
            "pipeline",
            [
                {"$addFields": {"copy": "$name", "bonus": {"$literal": 5}}},
                {"$unwind": "$scores"},
                {"$project": {"name": 1, "score": "$scores", "bonus": 1}},
            ],
        )
        names = {doc["name"] for doc in out}
        self.assertEqual(names, {"alpha", "beta"})
        scores = sorted(doc["score"] for doc in out)
        self.assertEqual(scores, [1, 2, 3])

    def test_aggregate_lookup_facet(self) -> None:
        client = MongoClientCpp()
        users = [
            {"_id": 1, "name": "alpha"},
            {"_id": 2, "name": "beta"},
        ]
        orders = [
            {"order_id": 101, "user_id": 1},
            {"order_id": 102, "user_id": 2},
            {"order_id": 103, "user_id": 1},
        ]
        client.insert_many("default", "users", users)
        client.insert_many("default", "orders", orders)
        out = client.aggregate(
            "default",
            "orders",
            [
                {"$lookup": {
                    "from": "users",
                    "localField": "user_id",
                    "foreignField": "_id",
                    "as": "user_docs",
                }},
            ],
        )
        self.assertEqual(out[0]["user_docs"][0]["name"], "alpha")
        facet = client.aggregate(
            "default",
            "orders",
            [
                {"$facet": {
                    "counts": [{"$sortByCount": "$user_id"}],
                    "total": [{"$count": "n"}],
                }},
            ],
        )
        self.assertEqual(facet[0]["total"][0]["n"], 3)

    def test_update_ops_upsert_replace(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "age": 10, "tags": [1, 2]},
            {"name": "beta", "age": 20},
        ]
        client.insert_many("default", "updates", docs)

        matched = client.update_one(
            "default",
            "updates",
            {"name": "alpha"},
            {"$rename": {"name": "label"}, "$currentDate": {"updated_at": True}},
        )
        self.assertEqual(matched.matched_count, 1)
        renamed = client.find("default", "updates", {"label": "alpha"}).to_list()
        self.assertEqual(len(renamed), 1)
        self.assertIsNotNone(renamed[0].get("updated_at"))

        client.update_one(
            "default",
            "updates",
            {"label": "alpha"},
            {"$push": {"tags": 3}, "$addToSet": {"tags": {"$each": [2, 4]}}},
        )
        pulled = client.update_one(
            "default",
            "updates",
            {"label": "alpha"},
            {"$pull": {"tags": {"$in": [1, 4]}}},
        )
        self.assertEqual(pulled.matched_count, 1)
        updated = client.find("default", "updates", {"label": "alpha"}).to_list()[0]
        self.assertEqual(updated["tags"], [2, 3])

        client.update_one(
            "default",
            "updates",
            {"label": "alpha"},
            {"$unset": ["age"]},
        )
        post_unset = client.find("default", "updates", {"label": "alpha"}).to_list()[0]
        self.assertIsNone(post_unset.get("age"))

        matched = client.update_one(
            "default",
            "updates",
            {"label": "gamma"},
            {"$set": {"label": "gamma"}, "$setOnInsert": {"age": 40}},
            upsert=True,
        )
        self.assertEqual(matched.matched_count, 0)
        upserted = client.find("default", "updates", {"label": "gamma"}).to_list()[0]
        self.assertEqual(upserted["age"], 40)

        matched = client.replace_one(
            "default",
            "updates",
            {"label": "alpha"},
            {"label": "alpha", "age": 99},
        )
        self.assertEqual(matched.matched_count, 1)
        replaced = client.find("default", "updates", {"label": "alpha"}).to_list()[0]
        self.assertEqual(replaced["age"], 99)
        self.assertIsNone(replaced.get("tags"))
        matched = client.update_one(
            "default",
            "updates",
            {"label": "alpha"},
            {"$setOnInsert": {"age": 12}},
        )
        self.assertEqual(matched.matched_count, 1)

    def test_find_one_and_delete(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "age": 10},
            {"name": "beta", "age": 20},
            {"name": "gamma", "age": 30},
        ]
        client.insert_many("default", "deletes", docs)
        deleted = client.find_one_and_delete(
            "default",
            "deletes",
            {"age": {"$gt": 10}},
            projection={"name": 1},
            sort=[("age", -1)],
        )
        self.assertEqual(deleted["name"], "gamma")
        remaining = client.find("default", "deletes", {}).to_list()
        self.assertEqual({doc["name"] for doc in remaining}, {"alpha", "beta"})
        deleted = client.delete_many("default", "deletes", {"age": {"$gt": 0}})
        self.assertEqual(deleted.deleted_count, 2)
        remaining = client.find("default", "deletes", {}).to_list()
        self.assertEqual(len(remaining), 0)

    def test_find_one_and_update(self) -> None:
        client = MongoClientCpp()
        docs = [
            {"name": "alpha", "age": 10},
            {"name": "beta", "age": 20},
        ]
        client.insert_many("default", "find_update", docs)
        before = client.find_one_and_update(
            "default",
            "find_update",
            {"age": {"$gt": 5}},
            {"$set": {"age": 30}},
            projection={"name": 1, "age": 1},
            sort=[("age", -1)],
            return_document="before",
        )
        self.assertEqual(before["age"], 20)
        after = client.find_one_and_update(
            "default",
            "find_update",
            {"name": "alpha"},
            {"$set": {"age": 15}},
            projection={"name": 1, "age": 1},
        )
        self.assertEqual(after["age"], 15)
        missing = client.find_one_and_update(
            "default",
            "find_update",
            {"name": "gamma"},
            {"$set": {"name": "gamma", "age": 42}},
            upsert=True,
        )
        self.assertIsNotNone(missing)

    def test_insert_many_bulk_write(self) -> None:
        client = MongoClientCpp()
        result = client.insert_many(
            "default",
            "bulk",
            [{"name": "alpha"}, {"name": "beta"}],
            ordered=False,
        )
        self.assertEqual(result.inserted_count, 2)
        bulk = client.bulk_write(
            "default",
            "bulk",
            [
                {"insert_one": {"name": "gamma"}},
                {"update_one": {"filter": {"name": "alpha"}, "update": {"$set": {"age": 10}}}},
                {"delete_one": {"filter": {"name": "beta"}}},
            ],
        )
        self.assertEqual(bulk.inserted_count, 1)
        self.assertEqual(bulk.matched_count, 1)
        remaining = client.find("default", "bulk", {}).to_list()
        self.assertEqual({doc["name"] for doc in remaining}, {"alpha", "gamma"})

    def test_compatibility_workflow(self) -> None:
        client = MongoClientCpp()
        client.insert_many(
            "default",
            "workflow",
            [{"name": "alpha", "score": 1}, {"name": "beta", "score": 2}],
        )
        updated = client.find_one_and_update(
            "default",
            "workflow",
            {"name": "alpha"},
            {"$set": {"score": 3}},
            projection={"name": 1, "score": 1},
        )
        self.assertEqual(updated["score"], 3)
        deleted = client.find_one_and_delete(
            "default",
            "workflow",
            {"name": "beta"},
        )
        self.assertEqual(deleted["name"], "beta")

if __name__ == "__main__":
    unittest.main()
