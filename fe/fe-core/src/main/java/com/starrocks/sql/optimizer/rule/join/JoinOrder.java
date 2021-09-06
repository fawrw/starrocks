// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

package com.starrocks.sql.optimizer.rule.join;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.analysis.JoinOperator;
import com.starrocks.common.Pair;
import com.starrocks.sql.optimizer.ExpressionContext;
import com.starrocks.sql.optimizer.Group;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.Utils;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.logical.LogicalJoinOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rule.transformation.JoinPredicateUtils;
import com.starrocks.sql.optimizer.statistics.StatisticsCalculator;
import com.starrocks.statistic.Constants;

import java.util.BitSet;
import java.util.List;
import java.util.Map;

public abstract class JoinOrder {
    /**
     * Like {@link OptExpression} or {@link com.starrocks.sql.optimizer.GroupExpression} ,
     * Description of an expression in the join order environment
     * left and right child of join expressions point to child groups
     */
    static class ExpressionInfo {
        public ExpressionInfo(OptExpression expr) {
            this.expr = expr;
        }

        public ExpressionInfo(OptExpression expr,
                              GroupInfo leftChild,
                              GroupInfo rightChild) {
            this.expr = expr;
            this.leftChildExpr = leftChild;
            this.rightChildExpr = rightChild;
        }

        OptExpression expr;
        GroupInfo leftChildExpr;
        GroupInfo rightChildExpr;

        public double getCost() {
            return cost;
        }

        double cost = -1L;
        double rowCount = -1L;
    }

    /**
     * Like {@link Group}, the atoms bitset could identify one group
     */
    static class GroupInfo {
        public GroupInfo(BitSet atoms) {
            this.atoms = atoms;
        }

        final BitSet atoms;
        ExpressionInfo bestExprInfo = null;
        double lowestExprCost = Double.MAX_VALUE;
    }

    /**
     * The join level from bottom to top
     * For A Join B Join C Join D
     * Level 1 groups are: A, B, C, D
     * Level 2 groups are: AB, AC, AD, BC ...
     * Level 3 groups are: ABC, ABD, BCD ...
     * Level 4 groups are: ABCD
     */
    static class JoinLevel {
        final int level;
        List<GroupInfo> groups = Lists.newArrayList();

        public JoinLevel(int level) {
            this.level = level;
        }
    }

    /**
     * The Edge represents the join on predicate
     * For A.id = B.id
     * The predicate is A.id = B.id,
     * The vertexes are A and B
     */
    static class Edge {
        final BitSet vertexes = new BitSet();
        final ScalarOperator predicate;

        public Edge(ScalarOperator predicate) {
            this.predicate = predicate;
        }
    }

    public JoinOrder(OptimizerContext context) {
        this.context = context;
    }

    protected final OptimizerContext context;
    protected int atomSize;
    protected final List<JoinLevel> joinLevels = Lists.newArrayList();
    protected final Map<BitSet, GroupInfo> bitSetToGroupInfo = Maps.newHashMap();

    protected int edgeSize;
    protected final List<Edge> edges = Lists.newArrayList();

    // Atom: A child of the Multi join. This could be a table or some
    // other operator like a group by or a full outer join.
    void init(List<OptExpression> atoms, List<ScalarOperator> predicates) {
        // 1. calculate statistics for each atom expression
        for (OptExpression atom : atoms) {
            calculateStatistics(atom);
        }

        // 2. build join graph
        atomSize = atoms.size();
        edgeSize = predicates.size();
        for (ScalarOperator predicate : predicates) {
            edges.add(new Edge(predicate));
        }
        computeEdgeCover(atoms);

        // 3. init join levels
        // For human read easily, the join level start with 1, not 0.
        for (int i = 0; i <= atomSize; ++i) {
            joinLevels.add(new JoinLevel(i));
        }

        // 4.init join group info
        JoinLevel atomLevel = joinLevels.get(1);
        for (int i = 0; i < atomSize; ++i) {
            BitSet atomBit = new BitSet();
            atomBit.set(i);
            ExpressionInfo atomExprInfo = new ExpressionInfo(atoms.get(i));
            computeCost(atomExprInfo, true);

            GroupInfo groupInfo = new GroupInfo(atomBit);
            groupInfo.bestExprInfo = atomExprInfo;
            groupInfo.lowestExprCost = atomExprInfo.cost;
            atomLevel.groups.add(groupInfo);
        }
    }

    public void reorder(List<OptExpression> atoms, List<ScalarOperator> predicates) {
        init(atoms, predicates);
        enumerate();
    }

    // Different join order algorithms should have different implementations
    protected abstract void enumerate();

    //Get reorder result
    public abstract List<OptExpression> getResult();

    // Use graph to represent the join expression:
    // The vertex represent the join node,
    // The edge represent the join predicate
    protected void computeEdgeCover(List<OptExpression> vertexes) {
        for (int i = 0; i < edgeSize; ++i) {
            ScalarOperator predicate = edges.get(i).predicate;
            ColumnRefSet predicateColumn = predicate.getUsedColumns();

            for (int j = 0; j < atomSize; ++j) {
                OptExpression atom = vertexes.get(j);
                ColumnRefSet outputColumns = atom.getOutputColumns();
                if (predicateColumn.isIntersect(outputColumns)) {
                    edges.get(i).vertexes.set(j);
                }
            }
        }
    }

    protected List<GroupInfo> getGroupForLevel(int level) {
        return joinLevels.get(level).groups;
    }

    protected void calculateStatistics(OptExpression expr) {
        // Avoid repeated calculate
        if (expr.getStatistics() != null) {
            return;
        }

        for (OptExpression child : expr.getInputs()) {
            calculateStatistics(child);
        }

        ExpressionContext expressionContext = new ExpressionContext(expr);
        StatisticsCalculator statisticsCalculator = new StatisticsCalculator(
                expressionContext, expr.getOutputColumns(),
                context.getColumnRefFactory(), context.getDumpInfo());
        statisticsCalculator.estimatorStats();
        expr.setStatistics(expressionContext.getStatistics());
    }

    protected void computeCost(ExpressionInfo exprInfo, boolean penaltyCross) {
        double cost = exprInfo.expr.getStatistics().getOutputRowCount();
        exprInfo.rowCount = cost;
        if (exprInfo.leftChildExpr != null) {
            cost += exprInfo.leftChildExpr.bestExprInfo.cost;
            cost += exprInfo.rightChildExpr.bestExprInfo.cost;
            LogicalJoinOperator joinOperator = (LogicalJoinOperator) exprInfo.expr.getOp();
            if (penaltyCross && joinOperator.getJoinType().isCrossJoin()) {
                cost *= Constants.CrossJoinCostGreedyPenalty;
            }
        }
        exprInfo.cost = cost;
    }

    protected ExpressionInfo buildJoinExpr(GroupInfo leftGroup, GroupInfo rightGroup) {
        ExpressionInfo leftExprInfo = leftGroup.bestExprInfo;
        ExpressionInfo rightExprInfo = rightGroup.bestExprInfo;
        Pair<ScalarOperator, ScalarOperator> predicates = buildInnerJoinPredicate(
                leftGroup.atoms, rightGroup.atoms);
        LogicalJoinOperator newJoin;
        if (predicates.first != null) {
            newJoin =
                    new LogicalJoinOperator(JoinOperator.INNER_JOIN, predicates.first);
        } else {
            newJoin =
                    new LogicalJoinOperator(JoinOperator.CROSS_JOIN, null);
        }
        newJoin.setPredicate(predicates.second);

        // In StarRocks, we only support hash join.
        // So we always use small table as right child
        if (leftExprInfo.rowCount < rightExprInfo.rowCount) {
            OptExpression joinExpr = OptExpression.create(newJoin, rightExprInfo.expr,
                    leftExprInfo.expr);
            return new ExpressionInfo(joinExpr, rightGroup, leftGroup);
        } else {
            OptExpression joinExpr = OptExpression.create(newJoin, leftExprInfo.expr,
                    rightExprInfo.expr);
            return new ExpressionInfo(joinExpr, leftGroup, rightGroup);
        }
    }

    private Pair<ScalarOperator, ScalarOperator> buildInnerJoinPredicate(BitSet left, BitSet right) {
        List<ScalarOperator> equalOnPredicates = Lists.newArrayList();
        List<ScalarOperator> otherPredicates = Lists.newArrayList();
        BitSet joinBitSet = new BitSet();
        joinBitSet.or(left);
        joinBitSet.or(right);
        for (int i = 0; i < edgeSize; ++i) {
            Edge edge = edges.get(i);
            if (contains(joinBitSet, edge.vertexes) &&
                    left.intersects(edge.vertexes) &&
                    right.intersects(edge.vertexes)) {
                if (JoinPredicateUtils.isEqualBinaryPredicate(edge.predicate)) {
                    equalOnPredicates.add(edge.predicate);
                } else {
                    otherPredicates.add(edge.predicate);
                }
            }
        }
        return new Pair<>(Utils.compoundAnd(equalOnPredicates), Utils.compoundAnd(otherPredicates));
    }

    private boolean contains(BitSet left, BitSet right) {
        return right.stream().allMatch(left::get);
    }
}