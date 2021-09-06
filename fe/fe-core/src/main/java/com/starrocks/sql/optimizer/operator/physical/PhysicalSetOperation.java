// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.
package com.starrocks.sql.optimizer.operator.physical;

import com.google.common.base.Objects;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;

import java.util.List;

public class PhysicalSetOperation extends PhysicalOperator {
    protected List<ColumnRefOperator> outputColumnRefOp;
    protected List<List<ColumnRefOperator>> childOutputColumns;

    public PhysicalSetOperation(OperatorType type, List<ColumnRefOperator> outputColumnRefOp,
                                List<List<ColumnRefOperator>> childOutputColumns) {
        super(type);
        this.outputColumnRefOp = outputColumnRefOp;
        this.childOutputColumns = childOutputColumns;
    }

    public List<ColumnRefOperator> getOutputColumnRefOp() {
        return outputColumnRefOp;
    }

    public List<List<ColumnRefOperator>> getChildOutputColumns() {
        return childOutputColumns;
    }

    @Override
    public ColumnRefSet getUsedColumns() {
        ColumnRefSet set = super.getUsedColumns();
        childOutputColumns.forEach(l -> l.forEach(set::union));
        return set;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }
        PhysicalSetOperation that = (PhysicalSetOperation) o;
        return Objects.equal(outputColumnRefOp, that.outputColumnRefOp) &&
                Objects.equal(childOutputColumns, that.childOutputColumns);
    }

    @Override
    public int hashCode() {
        return Objects.hashCode(outputColumnRefOp, childOutputColumns);
    }
}