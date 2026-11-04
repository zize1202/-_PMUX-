// SPDX-License-Identifier: MIT
// Local PMUX sampling-boundary rewrite; no standard pass or ABC is invoked.
#ifndef PMUX_SDFFE_BOUNDARY_H
#define PMUX_SDFFE_BOUNDARY_H
#include "pmux_resource_share.h"

namespace pmux_sdffe_boundary_detail {
using namespace Yosys;
using namespace pmux_resource_detail;
struct Plan { Cell *root=nullptr; std::vector<Cell*> ffs; std::vector<std::pair<Bit,Bit>> controls; };

inline bool integer_parameter(Cell *cell,IdString name,int expected)
{
    if(!cell->hasParam(name))return false;
    const auto &value=cell->getParam(name);
    if(value.empty()||!value.is_fully_def())return false;
    auto number=value.try_as_int(false);return number&&*number==expected;
}
inline bool signal_ok(Context &ctx,const Sig &s,bool allow_undef)
{
    if(ctx.protected_signal(s))return false;
    for(auto bit:s){
        if(!bit.wire){if(!allow_undef&&bit!=Bit(State::S0)&&bit!=Bit(State::S1))return false;continue;}
        if(ctx.multiple.count(bit)||(ctx.input_bits.count(bit)&&ctx.driver.count(bit)))return false;
        if(!ctx.input_bits.count(bit)&&!ctx.driver.count(bit))return false;
    }
    return true;
}
inline bool make_plan(Context &ctx,Cell *root,Plan &plan)
{
    if(root->type!=ID($pmux)||!ctx.module->selected(root)||protected_object(root))return false;
    for(auto port:{ID::A,ID::B,ID::S,ID::Y})if(!root->hasPort(port))return false;
    Sig y=ctx.sigmap(root->getPort(ID::Y));int width=GetSize(y),selects=GetSize(root->getPort(ID::S));
    if(width<1||width>64||selects<1||selects>64||!integer_parameter(root,ID::WIDTH,width)||
       !integer_parameter(root,ID::S_WIDTH,selects)||GetSize(root->getPort(ID::A))!=width||
       GetSize(root->getPort(ID::B))!=width*selects||ctx.whole_driver(y)!=root)return false;
    for(auto port:{ID::A,ID::B,ID::S,ID::Y})if(!signal_ok(ctx,ctx.sigmap(root->getPort(port)),true))return false;
    pool<Bit> ybits;pool<Cell*> consumers;
    for(auto bit:y){
        if(ctx.input_bits.count(bit)||ctx.output_bits.count(bit)||!ybits.insert(bit).second||!ctx.consumers.count(bit))return false;
        for(auto consumer:ctx.consumers.at(bit))consumers.insert(consumer);
    }
    if(consumers.empty()||consumers.size()>8)return false;
    for(auto ff:consumers){
        if(ff->type!=ID($sdffe)||!ctx.module->selected(ff)||protected_object(ff)||
           !integer_parameter(ff,ID::WIDTH,width)||!integer_parameter(ff,ID::CLK_POLARITY,1)||
           !integer_parameter(ff,ID::EN_POLARITY,1)||!integer_parameter(ff,ID::SRST_POLARITY,1)||
           !ff->hasParam(ID::SRST_VALUE))return false;
        const auto &reset=ff->getParam(ID::SRST_VALUE);
        if(GetSize(reset)!=width||!reset.is_fully_def())return false;
        for(auto bit:reset)if(bit!=State::S0)return false;
        for(auto port:{ID::D,ID::Q,ID::CLK,ID::EN,ID::SRST})if(!ff->hasPort(port))return false;
        if(ctx.sigmap(ff->getPort(ID::D))!=y||GetSize(ff->getPort(ID::Q))!=width||
           GetSize(ff->getPort(ID::CLK))!=1||GetSize(ff->getPort(ID::EN))!=1||GetSize(ff->getPort(ID::SRST))!=1)return false;
        // Consumer sets alone cannot exclude a simultaneous read through EN/CLK.
        for(auto &port:ff->connections()){
            if(port.first!=ID::D&&port.first!=ID::Q&&port.first!=ID::CLK&&port.first!=ID::EN&&port.first!=ID::SRST)return false;
            Sig value=ctx.sigmap(port.second);
            if(!signal_ok(ctx,value,port.first==ID::D))return false;
            if(port.first!=ID::D&&port.first!=ID::Q)for(auto bit:value)if(ybits.count(bit))return false;
        }
        Bit clock=ctx.sigmap(ff->getPort(ID::CLK))[0];if(!clock.wire)return false;
        pool<Bit> qbits;
        for(auto bit:ctx.sigmap(ff->getPort(ID::Q)))
            if(!bit.wire||ctx.input_bits.count(bit)||ctx.multiple.count(bit)||!qbits.insert(bit).second||
               !ctx.driver.count(bit)||ctx.driver.at(bit)!=ff||ybits.count(bit))return false;
        Bit en=ctx.sigmap(ff->getPort(ID::EN))[0],reset_signal=ctx.sigmap(ff->getPort(ID::SRST))[0];
        if(reset_signal<en)std::swap(en,reset_signal);
        plan.ffs.push_back(ff);plan.controls.emplace_back(en,reset_signal);
    }
    // Stable cell order; OR sharing itself is independent of candidate order.
    for(size_t i=0;i<plan.ffs.size();i++)for(size_t j=i+1;j<plan.ffs.size();j++)
        if(plan.ffs[j]->name<plan.ffs[i]->name){std::swap(plan.ffs[i],plan.ffs[j]);std::swap(plan.controls[i],plan.controls[j]);}
    plan.root=root;return true;
}
inline int commit(Module *module,const std::vector<Plan> &plans)
{
    std::map<std::pair<Bit,Bit>,Bit> shared;int changed=0;
    for(auto &plan:plans)for(size_t i=0;i<plan.ffs.size();i++){
        const auto &key=plan.controls[i];Bit enable;
        auto found=shared.find(key);
        if(found!=shared.end())enable=found->second;
        else{
            if(key.first==Bit(State::S1)||key.second==Bit(State::S1))enable=Bit(State::S1);
            else if(key.first==Bit(State::S0)||key.first==key.second)enable=key.second;
            else if(key.second==Bit(State::S0))enable=key.first;
            else{enable=Bit(module->addWire(NEW_ID));module->addOr(NEW_ID,key.first,key.second,enable);}
            shared[key]=enable;
        }
        Cell *ff=plan.ffs[i];ff->type=ID($sdffce);ff->setPort(ID::EN,enable);changed++;
        log("PMUX_SDFFE_BOUNDARY module=%s pmux=%s ff=%s width=%d reset_priority_to_enable_priority=1\n",log_id(module),log_id(plan.root),log_id(ff),GetSize(ff->getPort(ID::Q)));
    }
    return changed;
}
} // namespace pmux_sdffe_boundary_detail
#include "pmux_cycloneiv_sclr.h"
#include "pmux_cycloneiv_sload.h"

inline int pmux_restate_sdffe_boundaries(Yosys::RTLIL::Design *design,bool cycloneiv_sclr=false)
{
    using namespace Yosys;using namespace pmux_resource_detail;using namespace pmux_sdffe_boundary_detail;
    if(cycloneiv_sclr&&!pmux_cycloneiv_sclr_detail::primitive_contract(design)){log("PMUX_CYCLONEIV_SCLR_SKIP reason=missing-or-unsupported-primitive-contract fallback=sdffce\n");cycloneiv_sclr=false;}
    int examined=0,planned=0,changed=0;
    for(auto module:design->selected_modules()){
        if(module->get_blackbox_attribute()||protected_object(module))continue;
        std::vector<Cell*> roots;bool have_ff=false;
        for(auto cell:module->selected_cells()){
            if(cell->type==ID($pmux)&&roots.size()<64)roots.push_back(cell);
            if(cell->type==ID($sdffe))have_ff=true;
        }
        if(roots.empty()||!have_ff)continue;
        Context ctx(module);if(!ctx.complete)continue;
        std::vector<Plan> plans;pool<Cell*> used;int module_ffs=0;
        for(auto root:roots){
            if(examined++>=128||planned>=64)break;
            Plan plan;if(!make_plan(ctx,root,plan))continue;
            bool conflict=false;for(auto ff:plan.ffs)if(used.count(ff))conflict=true;
            if(conflict||module_ffs+GetSize(plan.ffs)>32||planned+GetSize(plan.ffs)>64)continue;
            for(auto ff:plan.ffs)used.insert(ff);
            module_ffs+=GetSize(plan.ffs);planned+=GetSize(plan.ffs);plans.push_back(plan);
        }
        // All Context queries finish before any FF port/type changes. No stale
        // driver/consumer index is queried during or after this module commit.
        pmux_cycloneiv_sclr_detail::Batch hardware;if(cycloneiv_sclr)hardware=pmux_cycloneiv_sclr_detail::prepare(module,ctx.sigmap,plans);
        std::vector<pmux_cycloneiv_sload_detail::LoadPlan> loads;if(cycloneiv_sclr)loads=pmux_cycloneiv_sload_detail::prepare(ctx,plans,hardware);
        changed+=commit(module,plans);
        if(cycloneiv_sclr){pmux_cycloneiv_sload_detail::commit(module,loads);pmux_cycloneiv_sclr_detail::commit(module,hardware);}
        if(examined>=128||planned>=64)break;
    }
    return changed;
}
#endif
