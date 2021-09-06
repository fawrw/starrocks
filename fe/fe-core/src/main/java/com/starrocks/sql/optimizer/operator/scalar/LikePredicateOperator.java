// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.
package com.starrocks.sql.optimizer.operator.scalar;

import com.google.common.base.Preconditions;
import com.starrocks.sql.optimizer.operator.OperatorType;

import java.util.Objects;

public class LikePredicateOperator extends PredicateOperator {
    private final LikeType likeType;

    public LikePredicateOperator(ScalarOperator... arguments) {
        super(OperatorType.LIKE, arguments);
        this.likeType = LikeType.LIKE;
        Preconditions.checkState(arguments.length == 2);
    }

    public LikePredicateOperator(LikeType likeType, ScalarOperator... arguments) {
        super(OperatorType.LIKE, arguments);
        this.likeType = likeType;
        Preconditions.checkState(arguments.length == 2);
    }

    public enum LikeType {
        LIKE,
        REGEXP
    }

    public boolean isRegexp() {
        return LikeType.REGEXP.equals(this.likeType);
    }

    public LikeType getLikeType() {
        return likeType;
    }

    @Override
    public String toString() {
        if (LikeType.LIKE.equals(likeType)) {
            return getChild(0).toString() + " LIKE " + getChild(1).toString();
        }

        return getChild(0).toString() + " REGEXP " + getChild(1).toString();
    }

    @Override
    public <R, C> R accept(ScalarOperatorVisitor<R, C> visitor, C context) {
        return visitor.visitLikePredicateOperator(this, context);
    }

    @Override
    public String debugString() {
        if (LikeType.LIKE.equals(likeType)) {
            return getChild(0).debugString() + " LIKE " + getChild(1).debugString();
        }

        return getChild(0).debugString() + " REGEXP " + getChild(1).debugString();
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }
        if (!super.equals(o)) {
            return false;
        }
        LikePredicateOperator that = (LikePredicateOperator) o;
        return likeType == that.likeType;
    }

    @Override
    public int hashCode() {
        return Objects.hash(super.hashCode(), likeType);
    }

    @Override
    public boolean isStrictPredicate() {
        return getChild(0).isColumnRefOrCast();
    }
}