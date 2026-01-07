#include <cassert>

#include "pcdb/aggregate.h"
#include "pcdb/dataset.h"
#include "pcdb/mask.h"
#include "pcdb/scan.h"

int main() {
    const double values[] = {1.0, 2.0, 3.0, 4.0};
    pcdb::AggregateResult result;
    pcdb::AggregateDouble(values, 4, nullptr, &result);
    assert(result.count == 4);
    assert(result.sum == 10.0);
    assert(result.min == 1.0);
    assert(result.max == 4.0);

    pcdb::Mask mask(4);
    mask.Set(1, true);
    mask.Set(3, true);
    pcdb::AggregateResult masked;
    pcdb::AggregateDouble(values, 4, &mask, &masked);
    assert(masked.count == 2);
    assert(masked.sum == 6.0);
    assert(masked.min == 2.0);
    assert(masked.max == 4.0);

    pcdb::Dataset users("users");
    users.AddField(pcdb::FieldVector("age", pcdb::FieldType::kInt32));
    assert(users.Append({pcdb::FieldValue::Int32(10)}));
    assert(users.Append({pcdb::FieldValue::Int32(20)}));
    assert(users.Append({pcdb::FieldValue::Null(pcdb::FieldType::kInt32)}));
    assert(users.Append({pcdb::FieldValue::Int32(40)}));

    pcdb::Mask age_mask(4);
    age_mask.Set(1, true);
    age_mask.Set(3, true);
    pcdb::AggregateResult agg;
    pcdb::AggregateField(users.Fields()[0], &age_mask, &agg);
    assert(agg.count == 2);
    assert(agg.sum == 60.0);
    assert(agg.min == 20.0);
    assert(agg.max == 40.0);

    pcdb::AggregateResult count_only;
    pcdb::AggregateCount(users.Fields()[0], &age_mask, &count_only);
    assert(count_only.count == agg.count);

    pcdb::AggregateResult sum_only;
    pcdb::AggregateSum(users.Fields()[0], &age_mask, &sum_only);
    assert(sum_only.sum == agg.sum);

    pcdb::AggregateResult minmax_only;
    pcdb::AggregateMinMax(users.Fields()[0], &age_mask, &minmax_only);
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
    pcdb::AggregateResult sum_pred;
    pcdb::AggregateSumPredicate(users.Fields()[0], predicate2, &pred_ctx2, &sum_pred);
    assert(sum_pred.sum == 60.0);

    pcdb::AggregateResult count_pred;
    pcdb::AggregateCountPredicate(users.Fields()[0], predicate2, &pred_ctx2, &count_pred);
    assert(count_pred.count == agg.count);

    pcdb::AggregateResult minmax_pred;
    pcdb::AggregateMinMaxPredicate(users.Fields()[0], predicate2, &pred_ctx2, &minmax_pred);
    assert(minmax_pred.min == agg.min);
    assert(minmax_pred.max == agg.max);

    struct PredCtx {
        const int64_t* data;
        int64_t threshold;
    } pred_ctx{ages_data, 25};
    auto predicate = [](size_t index, void* ctx) {
        const auto* pred = static_cast<const PredCtx*>(ctx);
        return pred->data[index] > pred->threshold;
    };
    pcdb::Mask built;
    pcdb::BuildMaskLoop(4, predicate, &pred_ctx, &built);

    pcdb::FieldVector scores("scores", pcdb::FieldType::kFloat64);
    scores.AppendFloat64(1.5);
    scores.AppendFloat64(2.5);
    scores.AppendFloat64(3.5);
    scores.AppendFloat64(4.5);
    pcdb::AggregateResult masked_scores;
    pcdb::AggregateField(scores, &built, &masked_scores);
    assert(masked_scores.count == 2);
    assert(masked_scores.sum == 8.0);

    return 0;
}
