#pragma once
#include <QueryPlan/PlanVisitor.h>
#include <Optimizer/Rewriter/Rewriter.h>
#include <QueryPlan/PlanVisitor.h>
#include "QueryPlan/CTEVisitHelper.h"


namespace DB
{
struct HintsVisitorContext
{
    Strings name_list = {};
};

using HintsList = std::vector<PlanHints>;

class HintsPropagator : public Rewriter
{
public:
    String name() const override { return "HintsPropagator"; }

private:
    void rewrite(QueryPlan & plan, ContextMutablePtr context) const override;
    bool isEnabled(ContextMutablePtr context) const override { return context->getSettingsRef().enable_hints_propagator; }
};

class HintsVisitor : public PlanNodeVisitor<void, HintsVisitorContext>
{
public:
    explicit HintsVisitor(ContextMutablePtr & /*context_*/, CTEInfo & cte_info_, PlanNodePtr & root)
        : post_order_cte_helper(cte_info_, root)
    {
    }

private:
    void visitProjectionNode(ProjectionNode & node, HintsVisitorContext &) override;
    void visitJoinNode(JoinNode & node, HintsVisitorContext &) override;
    void visitTableScanNode(TableScanNode & node, HintsVisitorContext &) override;
    void visitPlanNode(PlanNodeBase & node, HintsVisitorContext &) override;
    void visitFilterNode(FilterNode & node, HintsVisitorContext & hints_context) override;
    void visitCTERefNode(CTERefNode & node, HintsVisitorContext &) override;
    void visitAggregatingNode(AggregatingNode & node, HintsVisitorContext &) override;


    void attachPlanHints(PlanNodeBase & node, HintOptions & hint_options);
    void processNodeWithHints(PlanNodeBase & node, HintsVisitorContext & node_options, HintOptions & hint_options);

    CTEPostorderVisitHelper post_order_cte_helper;
    HintsList hints_list;
};

}
