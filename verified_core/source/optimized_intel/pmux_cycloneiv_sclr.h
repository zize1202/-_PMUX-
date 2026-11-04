// SPDX-License-Identifier: MIT
// Explicit Cyclone IV target only; use real ENA > SCLR hardware semantics.
#ifndef PMUX_CYCLONEIV_SCLR_H
#define PMUX_CYCLONEIV_SCLR_H
#include "kernel/ffinit.h"
namespace pmux_cycloneiv_sclr_detail {
using namespace Yosys;
using namespace pmux_resource_detail;
using pmux_sdffe_boundary_detail::Plan;
struct HardwareFF { Cell *ff=nullptr; Sig d,q,clock,reset,asdata; Bit sload=Bit(State::S0); RTLIL::Const init; };
struct Batch { std::vector<HardwareFF> ffs; std::vector<std::pair<RTLIL::Wire*,RTLIL::Const>> init_edits; };
inline bool primitive_contract(RTLIL::Design *design)
{
    Module *primitive=design->module(ID(dffeas));
    if(!primitive||!primitive->get_blackbox_attribute()||
       !primitive->parameter_default_values.count(ID(power_up))||!primitive->parameter_default_values.count(ID(is_wysiwyg)))return false;
    int ports=0;
    for(auto wire:primitive->wires())if(wire->port_input||wire->port_output)ports++;
    if(ports!=10)return false;
    for(auto name:{ID(q),ID(d),ID(clk),ID(clrn),ID(prn),ID(ena),ID(asdata),ID(aload),ID(sclr),ID(sload)}){
        auto wire=primitive->wire(name);bool output=name==ID(q);
        if(!wire||wire->width!=1||wire->port_output!=output||wire->port_input==output)return false;
    }
    return true;
}
inline Batch prepare(Module *module,const SigMap &sigmap,const std::vector<Plan> &plans)
{
    Batch batch;if(plans.empty())return batch;
    FfInitVals init(&sigmap,module); // Detects conflicting defined alias initial values.
    pool<Bit> encoded_bits;
    for(const auto &plan:plans)for(auto ff:plan.ffs){
        HardwareFF hw;hw.ff=ff;hw.d=ff->getPort(ID::D);hw.q=ff->getPort(ID::Q);
        hw.clock=ff->getPort(ID::CLK);hw.reset=ff->getPort(ID::SRST);hw.init=init(hw.q);
        if(!hw.init.is_fully_def()){
            log("PMUX_CYCLONEIV_SCLR_SKIP module=%s ff=%s reason=unknown-logical-init fallback=sdffce\n",log_id(module),log_id(ff));continue;
        }
        bool binary=true;for(auto bit:hw.init)if(bit!=State::S0&&bit!=State::S1)binary=false;
        if(!binary)continue;
        for(auto bit:sigmap(hw.q))encoded_bits.insert(bit);
        batch.ffs.push_back(hw);
    }
    // FfInitVals::remove_init clears one recorded origin. All same-value aliases
    // must instead be cleared together, and only for actually encoded Q bits.
    for(auto wire:module->wires()){
        auto found=wire->attributes.find(ID::init);if(found==wire->attributes.end())continue;
        RTLIL::Const value=found->second;Sig bits=sigmap(wire);bool changed=false;
        for(int i=0;i<GetSize(bits)&&i<GetSize(value);i++)if(encoded_bits.count(bits[i])&&value[i]!=State::Sx){value.set(i,State::Sx);changed=true;}
        if(changed)batch.init_edits.emplace_back(wire,value);
    }
    return batch;
}
inline int commit(Module *module,const Batch &batch)
{
    int bits=0;
    for(const auto &hw:batch.ffs){
        // r28's preceding commit supplies the shared EN|SRST. No Context or
        // driver/consumer queries are allowed after that commit.
        Sig enable=hw.ff->getPort(ID::EN);
        for(int i=0;i<GetSize(hw.q);i++){
            Cell *mapped=module->addCell(NEW_ID,ID(dffeas));mapped->attributes=hw.ff->attributes;
            mapped->setParam(ID(is_wysiwyg),RTLIL::Const("TRUE"));
            mapped->setParam(ID(power_up),RTLIL::Const(hw.init[i]==State::S1?"high":"low"));
            mapped->setPort(ID(d),hw.d[i]);mapped->setPort(ID(q),hw.q[i]);mapped->setPort(ID(clk),hw.clock);
            mapped->setPort(ID(ena),enable);mapped->setPort(ID(sclr),hw.reset);
            mapped->setPort(ID(clrn),State::S1);mapped->setPort(ID(prn),State::S1);
            mapped->setPort(ID(aload),State::S0);mapped->setPort(ID(asdata),hw.asdata.empty()?Bit(State::S0):hw.asdata[i]);mapped->setPort(ID(sload),hw.sload);bits++;
        }
        log("PMUX_CYCLONEIV_SCLR module=%s ff=%s bits=%d init=%s same_CLK_D_Q_SRST=1\n",log_id(module),log_id(hw.ff),GetSize(hw.q),log_signal(hw.init));
        module->remove(hw.ff);
    }
    for(const auto &edit:batch.init_edits){
        if(edit.second.is_fully_undef_x_only())edit.first->attributes.erase(ID::init);
        else edit.first->attributes[ID::init]=edit.second;
    }
    return bits;
}
} // namespace pmux_cycloneiv_sclr_detail
#endif
