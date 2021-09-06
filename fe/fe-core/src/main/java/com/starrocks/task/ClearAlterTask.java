// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/task/ClearAlterTask.java

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

package com.starrocks.task;

import com.starrocks.thrift.TClearAlterTaskRequest;
import com.starrocks.thrift.TTaskType;

public class ClearAlterTask extends AgentTask {
    private int schemaHash;

    public ClearAlterTask(long backendId, long dbId, long tableId, long partitionId, long indexId,
                          long tabletId, int schemaHash) {
        super(null, backendId, TTaskType.CLEAR_ALTER_TASK, dbId, tableId, partitionId, indexId, tabletId);

        this.schemaHash = schemaHash;
        this.isFinished = false;
    }

    public TClearAlterTaskRequest toThrift() {
        TClearAlterTaskRequest request = new TClearAlterTaskRequest(tabletId, schemaHash);
        return request;
    }

    public int getSchemaHash() {
        return schemaHash;
    }
}