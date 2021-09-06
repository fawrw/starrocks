// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/exprs/agg/aggregate_factory.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exprs/agg/aggregate_factory.h"

#include <unordered_map>

#include "column/type_traits.h"
#include "exprs/agg/avg.h"
#include "exprs/agg/bitmap_intersect.h"
#include "exprs/agg/bitmap_union.h"
#include "exprs/agg/bitmap_union_count.h"
#include "exprs/agg/bitmap_union_int.h"
#include "exprs/agg/count.h"
#include "exprs/agg/distinct.h"
#include "exprs/agg/group_concat.h"
#include "exprs/agg/hll_ndv.h"
#include "exprs/agg/hll_union.h"
#include "exprs/agg/hll_union_count.h"
#include "exprs/agg/intersect_count.h"
#include "exprs/agg/maxmin.h"
#include "exprs/agg/nullable_aggregate.h"
#include "exprs/agg/percentile_approx.h"
#include "exprs/agg/sum.h"
#include "exprs/agg/variance.h"
#include "exprs/agg/window.h"
#include "percentile_union.h"

namespace starrocks::vectorized {

// The function should be placed by alphabetical order

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeAvgAggregateFunction() {
    return std::make_shared<AvgAggregateFunction<PT>>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeBitmapUnionIntAggregateFunction() {
    return std::make_shared<BitmapUnionIntAggregateFunction<PT>>();
}

AggregateFunctionPtr AggregateFactory::MakeBitmapUnionAggregateFunction() {
    return std::make_shared<BitmapUnionAggregateFunction>();
}

AggregateFunctionPtr AggregateFactory::MakeBitmapIntersectAggregateFunction() {
    return std::make_shared<BitmapIntersectAggregateFunction>();
}

AggregateFunctionPtr AggregateFactory::MakeBitmapUnionCountAggregateFunction() {
    return std::make_shared<BitmapUnionCountAggregateFunction>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeIntersectCountAggregateFunction() {
    return std::make_shared<IntersectCountAggregateFunction<PT>>();
}

AggregateFunctionPtr AggregateFactory::MakeCountAggregateFunction() {
    return std::make_shared<CountAggregateFunction>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeCountDistinctAggregateFunction() {
    return std::make_shared<DistinctAggregateFunction<PT, AggDistinctType::COUNT>>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeGroupConcatAggregateFunction() {
    return std::make_shared<GroupConcatAggregateFunction<PT>>();
}

AggregateFunctionPtr AggregateFactory::MakeCountNullableAggregateFunction() {
    return std::make_shared<CountNullableAggregateFunction>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeMaxAggregateFunction() {
    return std::make_shared<MaxMinAggregateFunction<PT, MaxAggregateData<PT>, MaxElement<PT, MaxAggregateData<PT>>>>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeMinAggregateFunction() {
    return std::make_shared<MaxMinAggregateFunction<PT, MinAggregateData<PT>, MinElement<PT, MinAggregateData<PT>>>>();
}

template <typename NestedState>
AggregateFunctionPtr AggregateFactory::MakeNullableAggregateFunctionUnary(AggregateFunctionPtr nested_function) {
    using AggregateDataType = NullableAggregateFunctionState<NestedState>;
    return std::make_shared<NullableAggregateFunctionUnary<AggregateDataType>>(nested_function);
}

template <typename NestedState>
AggregateFunctionPtr AggregateFactory::MakeNullableAggregateFunctionVariadic(AggregateFunctionPtr nested_function) {
    using AggregateDataType = NullableAggregateFunctionState<NestedState>;
    return std::make_shared<NullableAggregateFunctionVariadic<AggregateDataType>>(nested_function);
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeSumAggregateFunction() {
    return std::make_shared<SumAggregateFunction<PT>>();
}

template <PrimitiveType PT, bool is_sample>
AggregateFunctionPtr AggregateFactory::MakeVarianceAggregateFunction() {
    return std::make_shared<VarianceAggregateFunction<PT, is_sample>>();
}

template <PrimitiveType PT, bool is_sample>
AggregateFunctionPtr AggregateFactory::MakeStddevAggregateFunction() {
    return std::make_shared<StddevAggregateFunction<PT, is_sample>>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeSumDistinctAggregateFunction() {
    return std::make_shared<DistinctAggregateFunction<PT, AggDistinctType::SUM>>();
}

AggregateFunctionPtr AggregateFactory::MakeHllUnionAggregateFunction() {
    return std::make_shared<HllUnionAggregateFunction>();
}

AggregateFunctionPtr AggregateFactory::MakeHllUnionCountAggregateFunction() {
    return std::make_shared<HllUnionCountAggregateFunction>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeHllNdvAggregateFunction() {
    return std::make_shared<HllNdvAggregateFunction<PT>>();
}

AggregateFunctionPtr AggregateFactory::MakePercentileApproxAggregateFunction() {
    return std::make_shared<PercentileApproxAggregateFunction>();
}

AggregateFunctionPtr AggregateFactory::MakePercentileUnionAggregateFunction() {
    return std::make_shared<PercentileUnionAggregateFunction>();
}

// Windows functions:

AggregateFunctionPtr AggregateFactory::MakeDenseRankWindowFunction() {
    return std::make_shared<DenseRankWindowFunction>();
}

AggregateFunctionPtr AggregateFactory::MakeRankWindowFunction() {
    return std::make_shared<RankWindowFunction>();
}

AggregateFunctionPtr AggregateFactory::MakeRowNumberWindowFunction() {
    return std::make_shared<RowNumberWindowFunction>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeFirstValueWindowFunction() {
    return std::make_shared<FirstValueWindowFunction<PT>>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeLastValueWindowFunction() {
    return std::make_shared<LastValueWindowFunction<PT>>();
}

template <PrimitiveType PT>
AggregateFunctionPtr AggregateFactory::MakeLeadLagWindowFunction() {
    return std::make_shared<LeadLagWindowFunction<PT>>();
}

// ----------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------

typedef std::tuple<std::string, int, int, bool> Quadruple;

struct AggregateFuncMapHash {
    size_t operator()(const Quadruple& quadruple) const {
        std::hash<std::string> hasher;
        return hasher(std::get<0>(quadruple)) ^ std::get<1>(quadruple) ^ std::get<2>(quadruple) ^
               std::get<3>(quadruple);
    }
};

class AggregateFuncResolver {
    DECLARE_SINGLETON(AggregateFuncResolver);

public:
    const AggregateFunction* get_aggregate_info(const std::string& name, const PrimitiveType arg_type,
                                                const PrimitiveType return_type, const bool is_null) const {
        auto pair = _infos_mapping.find(std::make_tuple(name, arg_type, return_type, is_null));
        if (pair == _infos_mapping.end()) {
            return nullptr;
        }
        return pair->second.get();
    }

    template <PrimitiveType arg_type, PrimitiveType return_type>
    void add_aggregate_mapping(std::string&& name) {
        _infos_mapping.emplace(std::make_tuple(name, arg_type, return_type, false),
                               create_function<arg_type, return_type, false>(name));
        _infos_mapping.emplace(std::make_tuple(name, arg_type, return_type, true),
                               create_function<arg_type, return_type, true>(name));
    }

    template <PrimitiveType arg_type, PrimitiveType return_type>
    void add_object_mapping(std::string&& name) {
        _infos_mapping.emplace(std::make_tuple(name, arg_type, return_type, false),
                               create_object_function<arg_type, return_type, false>(name));
        _infos_mapping.emplace(std::make_tuple(name, arg_type, return_type, true),
                               create_object_function<arg_type, return_type, true>(name));
    }

    template <PrimitiveType arg_type, PrimitiveType return_type, bool is_null>
    AggregateFunctionPtr create_object_function(std::string& name) {
        if constexpr (is_null) {
            if (name == "hll_raw_agg" || name == "hll_union") {
                auto hll_union = AggregateFactory::MakeHllUnionAggregateFunction();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<HyperLogLog>(hll_union);
            } else if (name == "hll_union_agg") {
                auto hll_union_count = AggregateFactory::MakeHllUnionCountAggregateFunction();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<HyperLogLog>(hll_union_count);
            } else if (name == "bitmap_union") {
                auto bitmap = AggregateFactory::MakeBitmapUnionAggregateFunction();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<BitmapValue>(bitmap);
            } else if (name == "bitmap_intersect") {
                auto bitmap = AggregateFactory::MakeBitmapIntersectAggregateFunction();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<BitmapValuePacked>(bitmap);
            } else if (name == "bitmap_union_count") {
                auto bitmap = AggregateFactory::MakeBitmapUnionCountAggregateFunction();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<BitmapValue>(bitmap);
            } else if (name == "intersect_count") {
                auto bitmap = AggregateFactory::MakeIntersectCountAggregateFunction<arg_type>();
                return AggregateFactory::MakeNullableAggregateFunctionVariadic<
                        BitmapIntersectAggregateState<BitmapRuntimeCppType<arg_type>>>(bitmap);
            } else if (name == "ndv" || name == "approx_count_distinct") {
                auto ndv = AggregateFactory::MakeHllNdvAggregateFunction<arg_type>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<HyperLogLog>(ndv);
            } else if (name == "percentile_union") {
                auto percentile = AggregateFactory::MakePercentileUnionAggregateFunction();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<PercentileValue>(percentile);
            }
        } else {
            if (name == "hll_raw_agg" || name == "hll_union") {
                return AggregateFactory::MakeHllUnionAggregateFunction();
            } else if (name == "hll_union_agg") {
                return AggregateFactory::MakeHllUnionCountAggregateFunction();
            } else if (name == "bitmap_union") {
                return AggregateFactory::MakeBitmapUnionAggregateFunction();
            } else if (name == "bitmap_intersect") {
                return AggregateFactory::MakeBitmapIntersectAggregateFunction();
            } else if (name == "bitmap_union_count") {
                return AggregateFactory::MakeBitmapUnionCountAggregateFunction();
            } else if (name == "intersect_count") {
                return AggregateFactory::MakeIntersectCountAggregateFunction<arg_type>();
            } else if (name == "ndv" || name == "approx_count_distinct") {
                return AggregateFactory::MakeHllNdvAggregateFunction<arg_type>();
            } else if (name == "percentile_union") {
                return AggregateFactory::MakePercentileUnionAggregateFunction();
            }
        }

        //MakeNullableAggregateFunctionUnary only support deal with single parameter aggregation function,
        //so here are the separate processing function percentile_approx
        if (name == "percentile_approx") {
            return AggregateFactory::MakePercentileApproxAggregateFunction();
        }

        return nullptr;
    }

    // TODO(kks): simplify create_function method
    template <PrimitiveType ArgPT, PrimitiveType ReturnPT, bool is_null>
    std::enable_if_t<isArithmeticPT<ArgPT>, AggregateFunctionPtr> create_function(std::string& name) {
        using ArgType = RunTimeCppType<ArgPT>;
        if constexpr (is_null) {
            if (name == "count") {
                return AggregateFactory::MakeCountNullableAggregateFunction();
            } else if (name == "sum") {
                AggregateFunctionPtr sum = AggregateFactory::MakeSumAggregateFunction<ArgPT>();
                using ResultType = RunTimeCppType<SumResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<SumAggregateState<ResultType>>(sum);
            } else if (name == "variance" || name == "variance_pop" || name == "var_pop") {
                auto variance = AggregateFactory::MakeVarianceAggregateFunction<ArgPT, false>();
                using ResultType = RunTimeCppType<DevFromAveResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DevFromAveAggregateState<ResultType>>(
                        variance);
            } else if (name == "variance_samp" || name == "var_samp") {
                auto variance = AggregateFactory::MakeVarianceAggregateFunction<ArgPT, true>();
                using ResultType = RunTimeCppType<DevFromAveResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DevFromAveAggregateState<ResultType>>(
                        variance);
            } else if (name == "std" || name == "stddev" || name == "stddev_pop") {
                auto stddev = AggregateFactory::MakeStddevAggregateFunction<ArgPT, false>();
                using ResultType = RunTimeCppType<DevFromAveResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DevFromAveAggregateState<ResultType>>(
                        stddev);
            } else if (name == "stddev_samp") {
                auto stddev = AggregateFactory::MakeStddevAggregateFunction<ArgPT, true>();
                using ResultType = RunTimeCppType<DevFromAveResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DevFromAveAggregateState<ResultType>>(
                        stddev);
            } else if (name == "bitmap_union_int") {
                auto bitmap = AggregateFactory::MakeBitmapUnionIntAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<BitmapValue>(bitmap);
            } else if (name == "max") {
                auto max = AggregateFactory::MakeMaxAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<MaxAggregateData<ArgPT>>(max);
            } else if (name == "min") {
                auto min = AggregateFactory::MakeMinAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<MinAggregateData<ArgPT>>(min);
            } else if (name == "avg") {
                auto avg = AggregateFactory::MakeAvgAggregateFunction<ArgPT>();
                using ResultType = RunTimeCppType<ImmediateAvgResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<AvgAggregateState<ResultType>>(avg);
            } else if (name == "multi_distinct_count") {
                auto distinct = AggregateFactory::MakeCountDistinctAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DistinctAggregateState<ArgPT>>(distinct);
            } else if (name == "multi_distinct_sum") {
                auto distinct = AggregateFactory::MakeSumDistinctAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DistinctAggregateState<ArgPT>>(distinct);
            } else if (name == "group_concat") {
                auto group_count = AggregateFactory::MakeGroupConcatAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionVariadic<GroupConcatAggregateState>(group_count);
            }
        } else {
            if (name == "count") {
                return AggregateFactory::MakeCountAggregateFunction();
            } else if (name == "sum") {
                return AggregateFactory::MakeSumAggregateFunction<ArgPT>();
            } else if (name == "variance" || name == "variance_pop" || name == "var_pop") {
                return AggregateFactory::MakeVarianceAggregateFunction<ArgPT, false>();
            } else if (name == "variance_samp" || name == "var_samp") {
                return AggregateFactory::MakeVarianceAggregateFunction<ArgPT, true>();
            } else if (name == "std" || name == "stddev" || name == "stddev_pop") {
                return AggregateFactory::MakeStddevAggregateFunction<ArgPT, false>();
            } else if (name == "stddev_samp") {
                return AggregateFactory::MakeStddevAggregateFunction<ArgPT, true>();
            } else if (name == "bitmap_union_int") {
                return AggregateFactory::MakeBitmapUnionIntAggregateFunction<ArgPT>();
            } else if (name == "max") {
                return AggregateFactory::MakeMaxAggregateFunction<ArgPT>();
            } else if (name == "min") {
                return AggregateFactory::MakeMinAggregateFunction<ArgPT>();
            } else if (name == "avg") {
                return AggregateFactory::MakeAvgAggregateFunction<ArgPT>();
            } else if (name == "multi_distinct_count") {
                return AggregateFactory::MakeCountDistinctAggregateFunction<ArgPT>();
            } else if (name == "multi_distinct_sum") {
                return AggregateFactory::MakeSumDistinctAggregateFunction<ArgPT>();
            } else if (name == "group_concat") {
                return AggregateFactory::MakeGroupConcatAggregateFunction<ArgPT>();
            }
        }

        if (name == "lead" || name == "lag") {
            return AggregateFactory::MakeLeadLagWindowFunction<ArgPT>();
        } else if (name == "first_value") {
            return AggregateFactory::MakeFirstValueWindowFunction<ArgPT>();
        } else if (name == "last_value") {
            return AggregateFactory::MakeLastValueWindowFunction<ArgPT>();
        } else if (name == "dense_rank") {
            return AggregateFactory::MakeDenseRankWindowFunction();
        } else if (name == "rank") {
            return AggregateFactory::MakeRankWindowFunction();
        } else if (name == "row_number") {
            return AggregateFactory::MakeRowNumberWindowFunction();
        }
        return nullptr;
    }

    template <PrimitiveType ArgPT, PrimitiveType ReturnPT, bool is_null>
    std::enable_if_t<!isArithmeticPT<ArgPT>, AggregateFunctionPtr> create_function(std::string& name) {
        using ArgType = RunTimeCppType<ArgPT>;
        if constexpr (is_null) {
            if (name == "avg") {
                auto avg = AggregateFactory::MakeAvgAggregateFunction<ArgPT>();
                using ResultType = RunTimeCppType<ImmediateAvgResultPT<ArgPT>>;
                return AggregateFactory::MakeNullableAggregateFunctionUnary<AvgAggregateState<ResultType>>(avg);
            } else if (name == "max") {
                auto max = AggregateFactory::MakeMaxAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<MaxAggregateData<ArgPT>>(max);
            } else if (name == "min") {
                auto min = AggregateFactory::MakeMinAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<MinAggregateData<ArgPT>>(min);
            } else if (name == "multi_distinct_count") {
                auto distinct = AggregateFactory::MakeCountDistinctAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionUnary<DistinctAggregateState<ArgPT>>(distinct);
            } else if (name == "group_concat") {
                auto group_count = AggregateFactory::MakeGroupConcatAggregateFunction<ArgPT>();
                return AggregateFactory::MakeNullableAggregateFunctionVariadic<GroupConcatAggregateState>(group_count);
            }
        } else {
            if (name == "avg") {
                return AggregateFactory::MakeAvgAggregateFunction<ArgPT>();
            } else if (name == "max") {
                return AggregateFactory::MakeMaxAggregateFunction<ArgPT>();
            } else if (name == "min") {
                return AggregateFactory::MakeMinAggregateFunction<ArgPT>();
            } else if (name == "multi_distinct_count") {
                return AggregateFactory::MakeCountDistinctAggregateFunction<ArgPT>();
            } else if (name == "group_concat") {
                return AggregateFactory::MakeGroupConcatAggregateFunction<ArgPT>();
            }
        }

        if (name == "lead" || name == "lag") {
            return AggregateFactory::MakeLeadLagWindowFunction<ArgPT>();
        } else if (name == "first_value") {
            return AggregateFactory::MakeFirstValueWindowFunction<ArgPT>();
        } else if (name == "last_value") {
            return AggregateFactory::MakeLastValueWindowFunction<ArgPT>();
        }
        return nullptr;
    }

private:
    std::unordered_map<Quadruple, AggregateFunctionPtr, AggregateFuncMapHash> _infos_mapping;
    DISALLOW_COPY_AND_ASSIGN(AggregateFuncResolver);
};

#define ADD_ALL_TYPE(FUNCTIONNAME)                                       \
    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_BOOLEAN>(FUNCTIONNAME);     \
    add_aggregate_mapping<TYPE_TINYINT, TYPE_TINYINT>(FUNCTIONNAME);     \
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_SMALLINT>(FUNCTIONNAME);   \
    add_aggregate_mapping<TYPE_INT, TYPE_INT>(FUNCTIONNAME);             \
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>(FUNCTIONNAME);       \
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_LARGEINT>(FUNCTIONNAME);   \
    add_aggregate_mapping<TYPE_FLOAT, TYPE_FLOAT>(FUNCTIONNAME);         \
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>(FUNCTIONNAME);       \
    add_aggregate_mapping<TYPE_VARCHAR, TYPE_VARCHAR>(FUNCTIONNAME);     \
    add_aggregate_mapping<TYPE_CHAR, TYPE_CHAR>(FUNCTIONNAME);           \
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>(FUNCTIONNAME); \
    add_aggregate_mapping<TYPE_DATETIME, TYPE_DATETIME>(FUNCTIONNAME);   \
    add_aggregate_mapping<TYPE_DATE, TYPE_DATE>(FUNCTIONNAME);           \
    add_aggregate_mapping<TYPE_DECIMAL32, TYPE_DECIMAL32>(FUNCTIONNAME); \
    add_aggregate_mapping<TYPE_DECIMAL64, TYPE_DECIMAL64>(FUNCTIONNAME); \
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>(FUNCTIONNAME);

AggregateFuncResolver::AggregateFuncResolver() {
    // The function should be placed by alphabetical order

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("avg");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("avg");
    add_aggregate_mapping<TYPE_DATETIME, TYPE_DATETIME>("avg");
    add_aggregate_mapping<TYPE_DATE, TYPE_DATE>("avg");
    add_aggregate_mapping<TYPE_DECIMAL32, TYPE_DECIMAL128>("avg");
    add_aggregate_mapping<TYPE_DECIMAL64, TYPE_DECIMAL128>("avg");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("avg");

    add_aggregate_mapping<TYPE_TINYINT, TYPE_BIGINT>("bitmap_union_int");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_BIGINT>("bitmap_union_int");
    add_aggregate_mapping<TYPE_INT, TYPE_BIGINT>("bitmap_union_int");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("bitmap_union_int");

    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("count");

    ADD_ALL_TYPE("max");
    ADD_ALL_TYPE("min");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_INT, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_CHAR, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_VARCHAR, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DATETIME, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DATE, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DECIMAL32, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DECIMAL64, TYPE_BIGINT>("multi_distinct_count");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_BIGINT>("multi_distinct_count");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_BIGINT>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_BIGINT>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_BIGINT>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_INT, TYPE_BIGINT>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_LARGEINT>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_DECIMAL32, TYPE_DECIMAL64>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_DECIMAL64, TYPE_DECIMAL64>("multi_distinct_sum");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("multi_distinct_sum");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_BIGINT>("sum");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_BIGINT>("sum");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_BIGINT>("sum");
    add_aggregate_mapping<TYPE_INT, TYPE_BIGINT>("sum");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_LARGEINT>("sum");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("sum");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("sum");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("sum");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("sum");
    add_aggregate_mapping<TYPE_DECIMAL32, TYPE_DECIMAL64>("sum");
    add_aggregate_mapping<TYPE_DECIMAL64, TYPE_DECIMAL64>("sum");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("sum");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("variance");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("variance");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("variance");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("variance_pop");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("variance_pop");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("variance_pop");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("var_pop");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("var_pop");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("var_pop");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("variance_samp");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("variance_samp");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("variance_samp");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("var_samp");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("var_samp");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("var_samp");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("std");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("std");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("std");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("stddev");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("stddev");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("stddev");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("stddev_pop");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("stddev_pop");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("stddev_pop");

    add_aggregate_mapping<TYPE_BOOLEAN, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_TINYINT, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_SMALLINT, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_INT, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_LARGEINT, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_FLOAT, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("stddev_samp");
    add_aggregate_mapping<TYPE_DECIMALV2, TYPE_DECIMALV2>("stddev_samp");
    add_aggregate_mapping<TYPE_DECIMAL128, TYPE_DECIMAL128>("stddev_samp");

    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("dense_rank");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("rank");
    add_aggregate_mapping<TYPE_BIGINT, TYPE_BIGINT>("row_number");

    add_aggregate_mapping<TYPE_CHAR, TYPE_VARCHAR>("group_concat");
    add_aggregate_mapping<TYPE_VARCHAR, TYPE_VARCHAR>("group_concat");

    ADD_ALL_TYPE("first_value");
    ADD_ALL_TYPE("last_value");
    ADD_ALL_TYPE("lead");
    ADD_ALL_TYPE("lag");

    add_object_mapping<TYPE_HLL, TYPE_HLL>("hll_union");
    add_object_mapping<TYPE_HLL, TYPE_HLL>("hll_raw_agg");
    add_object_mapping<TYPE_HLL, TYPE_BIGINT>("hll_union_agg");

    add_object_mapping<TYPE_OBJECT, TYPE_OBJECT>("bitmap_union");
    add_object_mapping<TYPE_OBJECT, TYPE_BIGINT>("bitmap_union_count");

    // This first type is the second type input of intersect_count.
    // And the first type is Bitmap.
    add_object_mapping<TYPE_TINYINT, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_SMALLINT, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_INT, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_BIGINT, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_LARGEINT, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_FLOAT, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_DOUBLE, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_DATE, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_DATETIME, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_DECIMALV2, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_CHAR, TYPE_BIGINT>("intersect_count");
    add_object_mapping<TYPE_VARCHAR, TYPE_BIGINT>("intersect_count");

    add_object_mapping<TYPE_OBJECT, TYPE_OBJECT>("bitmap_intersect");

    add_object_mapping<TYPE_BOOLEAN, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_TINYINT, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_SMALLINT, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_INT, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_BIGINT, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_LARGEINT, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_FLOAT, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DOUBLE, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_CHAR, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_VARCHAR, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DECIMALV2, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DATETIME, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DATE, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DECIMAL32, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DECIMAL64, TYPE_BIGINT>("ndv");
    add_object_mapping<TYPE_DECIMAL128, TYPE_BIGINT>("ndv");

    add_object_mapping<TYPE_BOOLEAN, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_TINYINT, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_SMALLINT, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_INT, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_BIGINT, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_LARGEINT, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_FLOAT, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DOUBLE, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_CHAR, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_VARCHAR, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DECIMALV2, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DATETIME, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DATE, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DECIMAL32, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DECIMAL64, TYPE_BIGINT>("approx_count_distinct");
    add_object_mapping<TYPE_DECIMAL128, TYPE_BIGINT>("approx_count_distinct");

    add_object_mapping<TYPE_BIGINT, TYPE_DOUBLE>("percentile_approx");
    add_object_mapping<TYPE_DOUBLE, TYPE_DOUBLE>("percentile_approx");

    add_object_mapping<TYPE_PERCENTILE, TYPE_PERCENTILE>("percentile_union");
}

#undef ADD_ALL_TYPE

AggregateFuncResolver::~AggregateFuncResolver() = default;

const AggregateFunction* get_aggregate_function(const std::string& name, PrimitiveType arg_type,
                                                PrimitiveType return_type, bool is_null) {
    return AggregateFuncResolver::instance()->get_aggregate_info(name, arg_type, return_type, is_null);
}

} // namespace starrocks::vectorized