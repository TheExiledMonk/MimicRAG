#include <cassert>

#include "mimicdb/aggregate.h"
#include "mimicdb/dataset.h"
#include "mimicdb/mask.h"
#include "mimicdb/scan.h"

int main() {
    const double values[] = {1.0, 2.0, 3.0, 4.0};
    mimicdb::AggregateResult result;
    mimicdb::AggregateDouble(values, 4, nullptr, &result);
    assert(result.count == 4);
    assert(result.sum == 10.0);
    assert(result.min == 1.0);
    assert(result.max == 4.0);

    mimicdb::Mask mask(4);
    mask.Set(1, true);
    mask.Set(3, true);
    mimicdb::AggregateResult masked;
    mimicdb::AggregateDouble(values, 4, &mask, &masked);
    assert(masked.count == 2);
    assert(masked.sum == 6.0);
    assert(masked.min == 2.0);
    assert(masked.max == 4.0);

    mimicdb::Dataset users("users");
    users.AddField(mimicdb::FieldVector("age", mimicdb::FieldType::kInt32));
    assert(users.Append({mimicdb::FieldValue::Int32(10)}));
    assert(users.Append({mimicdb::FieldValue::Int32(20)}));
    assert(users.Append({mimicdb::FieldValue::Null(mimicdb::FieldType::kInt32)}));
    assert(users.Append({mimicdb::FieldValue::Int32(40)}));

    mimicdb::Mask age_mask(4);
    age_mask.Set(1, true);
    age_mask.Set(3, true);
    mimicdb::AggregateResult agg;
    mimicdb::AggregateField(users.Fields()[0], &age_mask, &agg);
    assert(agg.count == 2);
    assert(agg.sum == 60.0);
    assert(agg.min == 20.0);
    assert(agg.max == 40.0);

    mimicdb::AggregateResult count_only;
    mimicdb::AggregateCount(users.Fields()[0], &age_mask, &count_only);
    assert(count_only.count == agg.count);

    mimicdb::AggregateResult sum_only;
    mimicdb::AggregateSum(users.Fields()[0], &age_mask, &sum_only);
    assert(sum_only.sum == agg.sum);

    mimicdb::AggregateResult minmax_only;
    mimicdb::AggregateMinMax(users.Fields()[0], &age_mask, &minmax_only);
    assert(minmax_only.min == agg.min);
    assert(minmax_only.max == agg.max);

    const int64_t ages_data[] = {10, 20, 30, 40};
    struct PredCtx2 {
        const int64_t* data;
        int64_t threshold;
    } pred_ctx2{ages_data, 25};
    auto predicate2 = [](size_t index, void* ctx) {
        const auto* pred = static_cast<const PredCtx2*>(ctx);
        return pred->data[index] > pred->threshold;
    };
    mimicdb::AggregateResult sum_pred;
    mimicdb::AggregateSumPredicate(users.Fields()[0], predicate2, &pred_ctx2, &sum_pred);
    assert(sum_pred.sum == 40.0);

    mimicdb::AggregateResult count_pred;
    mimicdb::AggregateCountPredicate(users.Fields()[0], predicate2, &pred_ctx2, &count_pred);
    assert(count_pred.count == 1);

    mimicdb::AggregateResult minmax_pred;
    mimicdb::AggregateMinMaxPredicate(users.Fields()[0], predicate2, &pred_ctx2, &minmax_pred);
    assert(minmax_pred.min == 40.0);
    assert(minmax_pred.max == 40.0);

    struct PredCtx {
        const int64_t* data;
        int64_t threshold;
    } pred_ctx{ages_data, 25};
    auto predicate = [](size_t index, void* ctx) {
        const auto* pred = static_cast<const PredCtx*>(ctx);
        return pred->data[index] > pred->threshold;
    };
    mimicdb::Mask built;
    mimicdb::BuildMaskLoop(4, predicate, &pred_ctx, &built);

    mimicdb::FieldVector scores("scores", mimicdb::FieldType::kFloat64);
    scores.AppendFloat64(1.5);
    scores.AppendFloat64(2.5);
    scores.AppendFloat64(3.5);
    scores.AppendFloat64(4.5);
    mimicdb::AggregateResult masked_scores;
    mimicdb::AggregateField(scores, &built, &masked_scores);
    assert(masked_scores.count == 2);
    assert(masked_scores.sum == 8.0);

    return 0;
}
