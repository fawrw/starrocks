// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/exprs/percentile_function.h

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

#ifndef STARROCKS_PERCENTILE_FUNCTION_H
#define STARROCKS_PERCENTILE_FUNCTION_H

#include "udf/udf.h"
#include "util/percentile_value.h"

namespace starrocks {
struct PercentileApproxState {
public:
    PercentileApproxState() : percentile(new PercentileValue()) {}

    ~PercentileApproxState() { delete percentile; }

    PercentileValue* percentile = nullptr;
    double targetQuantile = -1.0;
};

class PercentileFunctions {
public:
    static void init();

    static StringVal percentile_hash(FunctionContext* ctx, const DoubleVal& dest_base);

    static StringVal percentile_empty(FunctionContext* ctx);

    static void percentile_approx_update(FunctionContext* ctx, const StringVal& src, const DoubleVal& quantile,
                                         StringVal* dst);

    static DoubleVal percentile_approx_raw(FunctionContext* ctx, const StringVal& src, const DoubleVal& quantile);
};

} // namespace starrocks
#endif