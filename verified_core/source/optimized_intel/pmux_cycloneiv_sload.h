// SPDX-License-Identifier: MIT
// Fixed 3-arm private PMUX boundary: move one zero-logic-cost data leaf to SLOAD.
// Only the explicit Cyclone IV path calls this; no standard pass is invoked.
#ifndef PMUX_CYCLONEIV_SLOAD_H
#define PMUX_CYCLONEIV_SLOAD_H
#include "kernel/ff.h"
namespace pmux_cycloneiv_sload_detail {
using namespace Yosys;
using namespace pmux_resource_detail;
using pmux_sdffe_boundary_detail::Plan;
using pmux_cycloneiv_sclr_detail::Batch;
struct LoadPlan { Cell *root=nullptr; Sig remaining_data,remaining_select; int slot=-1; const char *kind=nullptr; };

inline const char *leaf_kind(Context &ctx,const Sig &value)
{
    if(value.empty()||ctx.protected_signal(value))return nullptr;
    pool<Bit> seen;bool all_inputs=true;Cell *producer=nullptr;
    for(auto bit:value){
        if(!bit.wire||ctx.multiple.count(bit)||!seen.insert(bit).second)return nullptr;
        if(ctx.input_bits.count(bit)){if(ctx.driver.count(bit))return nullptr;}
        else{
            all_inputs=false;if(!ctx.driver.count(bit))return nullptr;
            auto current=ctx.driver.at(bit);if(producer&&producer!=current)return nullptr;producer=current;
        }
    }
    if(all_inputs)return "primary-input";
    // A mixed PI/Q word, a Q slice, a latch, and a combinational result are not leaves.
    for(auto bit:value)if(ctx.input_bits.count(bit))return nullptr;
    if(!producer||!producer->is_builtin_ff()||protected_object(producer)||
       !producer->hasPort(ID::Q)||ctx.sigmap(producer->getPort(ID::Q))!=value)return nullptr;
    FfData ff(nullptr,producer);
    if(!ff.has_clk||GetSize(ff.sig_clk)!=1)return nullptr;
    return "clocked-ff-q";
}

inline std::vector<LoadPlan> prepare(Context &ctx,const std::vector<Plan> &plans,Batch &hardware)
{
    std::vector<LoadPlan> loads;dict<Cell*,int> mapped;
    for(int i=0;i<GetSize(hardware.ffs);i++)mapped[hardware.ffs[i].ff]=i;
    // The outer sampling-boundary planner already caps roots/FFs/module/design.
    for(const auto &plan:plans){
        auto root=plan.root;Sig select=ctx.sigmap(root->getPort(ID::S));
        if(GetSize(select)!=3)continue;
        bool complete=true;for(auto ff:plan.ffs)if(!mapped.count(ff))complete=false;
        if(!complete)continue; // Never remove data still observed by an unmapped FF.
        SelectionProof proof;if(!prove_selection(ctx,root,select,proof))continue;
        if(GetSize(proof.reachable)!=3||!proof.reachable[0]||!proof.reachable[1]||!proof.reachable[2])continue;
        int width=GetSize(root->getPort(ID::Y)),leaf=-1;const char *kind=nullptr;bool multiple=false;
        Sig data=ctx.sigmap(root->getPort(ID::B));
        for(int slot=0;slot<3;slot++)if(auto found=leaf_kind(ctx,data.extract(slot*width,width))){
            if(leaf>=0){multiple=true;break;}leaf=slot;kind=found;
        }
        if(multiple||leaf<0)continue;
        LoadPlan load;load.root=root;load.slot=leaf;load.kind=kind;
        // Retain the actual port bits and order, not a reconstructed truth table.
        Sig original_data=root->getPort(ID::B),original_select=root->getPort(ID::S);
        for(int slot=0;slot<3;slot++)if(slot!=leaf){
            load.remaining_data.append(original_data.extract(slot*width,width));
            load.remaining_select.append(original_select[slot]);
        }
        for(auto ff:plan.ffs){auto &hw=hardware.ffs[mapped.at(ff)];
            hw.asdata=original_data.extract(leaf*width,width);hw.sload=original_select[leaf];}
        loads.push_back(load);
    }
    return loads;
}

inline void commit(Module *module,const std::vector<LoadPlan> &loads)
{
    // All eligibility, alias, consumer, initial-state and AMO queries are over.
    // The existing root A/Y/attributes survive; later recovery builds a new Net.
    for(const auto &load:loads){
        load.root->setPort(ID::B,load.remaining_data);load.root->setPort(ID::S,load.remaining_select);
        load.root->setParam(ID::S_WIDTH,2);
        log("PMUX_CYCLONEIV_SLOAD module=%s pmux=%s slot=%d leaf=%s remaining_arms=2 same_default=1\n",log_id(module),log_id(load.root),load.slot,load.kind);
    }
}
} // namespace pmux_cycloneiv_sload_detail
#endif
