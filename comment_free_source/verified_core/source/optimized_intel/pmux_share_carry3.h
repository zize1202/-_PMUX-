// SPDX-License-Identifier: MIT
// Exact, deliberately narrow 3-bit XOR/ADD/SUB sharing before technology mapping.
#ifndef PMUX_SHARE_CARRY3_H
#define PMUX_SHARE_CARRY3_H
#include "pmux_resource_share.h"

namespace pmux_carry3_detail {
using namespace Yosys;
using namespace pmux_resource_detail;
inline bool matched_binary_signs(Cell *cell);
struct Plan {
    Cell *root = nullptr;
    Arm xor_arm, add_arm, sub_arm;
    Sig data, select, common;
};

inline bool scalar_conflict(Context &ctx, const Sig &value, Cell *root)
{
    for (auto bit : value) {
        if (ctx.multiple.count(bit)) return true;
        if (bit.wire && ctx.driver.count(bit) &&
            (ctx.input_bits.count(bit) || ctx.driver.at(bit) == root)) return true;
    }
    return false;
}

inline bool make_plan(Context &ctx, Cell *root, Plan &plan)
{
    if (root->type != ID($pmux) || protected_object(root) ||
        root->getParam(ID::WIDTH).as_int() != 3 || root->getParam(ID::S_WIDTH).as_int() != 3)
        return false;
    Sig a = ctx.sigmap(root->getPort(ID::A)), b = ctx.sigmap(root->getPort(ID::B));
    Sig s = ctx.sigmap(root->getPort(ID::S)), y = ctx.sigmap(root->getPort(ID::Y));
    if (GetSize(a) != 3 || GetSize(b) != 9 || GetSize(s) != 3 || GetSize(y) != 3 ||
        !known_literals(s) || ctx.protected_signal(y) || ctx.protected_signal(s) ||
        ctx.whole_driver(y) != root || scalar_conflict(ctx,s,root)) return false;
    SelectionProof proof;
    if (!prove_selection(ctx,root,s,proof)) return false;
    for (bool reachable : proof.reachable) if (!reachable) return false;
    std::vector<Arm> arms(3);
    pool<Cell *> operators;
    bool have_xor=false,have_add=false,have_sub=false;
    for (int slot=0;slot<3;slot++) {
        Arm &arm=arms[slot]; Sig value=b.extract(3*slot,3);
        if (!read_arm(ctx,root,slot,value,3,arm) || !ctx.module->selected(arm.operation) ||
            !matched_binary_signs(arm.operation) ||
                                                                             
            scalar_conflict(ctx,arm.left,root) || scalar_conflict(ctx,arm.right,root) ||
            !operators.insert(arm.operation).second) return false;
                                                                             
                                                                             
        for (auto bit : value) {
            int uses=0;
            for (auto id : {ID::A,ID::B,ID::S})
                for (auto used : ctx.sigmap(root->getPort(id))) if (used==bit) uses++;
            if (uses!=1) return false;
        }
        if (arm.kind==ID($xor)) { if(have_xor)return false;have_xor=true;plan.xor_arm=arm; }
        if (arm.kind==ID($add)) { if(have_add)return false;have_add=true;plan.add_arm=arm; }
        if (arm.kind==ID($sub)) { if(have_sub)return false;have_sub=true;plan.sub_arm=arm; }
    }
    if (!have_xor || !have_add || !have_sub) return false;
    plan.common=plan.sub_arm.right;
    for (auto arm : {&plan.xor_arm,&plan.add_arm}) {
        if (arm->right==plan.common) continue;
        if (arm->left!=plan.common) return false;
        std::swap(arm->left,arm->right);
    }
    plan.root=root;plan.data=b;plan.select=s;
    return true;
}

inline void emit(Context &ctx,const Plan &plan)
{
    Module *m=ctx.module;
    Bit add=plan.select[plan.add_arm.slot],sub=plan.select[plan.sub_arm.slot],mode_xor=plan.select[plan.xor_arm.slot];
    Sig add_or_sub(m->addWire(NEW_ID,3)),selected(m->addWire(NEW_ID,3));
    m->addMux(NEW_ID,plan.sub_arm.left,plan.add_arm.left,add,add_or_sub);
    m->addMux(NEW_ID,add_or_sub,plan.xor_arm.left,mode_xor,selected);
    Bit arith(m->addWire(NEW_ID));m->addOr(NEW_ID,add,sub,arith);
    Sig result;
    Bit carry(State::S0);
    for (int bit=0;bit<3;bit++) {
        Bit aq(m->addWire(NEW_ID));m->addXor(NEW_ID,selected[bit],plan.common[bit],aq);
        if (bit==0) result.append(aq);
        else { Bit out(m->addWire(NEW_ID));m->addXor(NEW_ID,aq,carry,out);result.append(out); }
        if (bit==2) break;                                                        
        Bit asub(m->addWire(NEW_ID)),generate(m->addWire(NEW_ID));
        m->addXor(NEW_ID,selected[bit],sub,asub);
        m->addAnd(NEW_ID,asub,plan.common[bit],generate);
        Bit sum=generate;
        if (bit!=0) {
            Bit propagate(m->addWire(NEW_ID)),incoming(m->addWire(NEW_ID)),combined(m->addWire(NEW_ID));
            m->addXor(NEW_ID,asub,plan.common[bit],propagate);
            m->addAnd(NEW_ID,propagate,carry,incoming);
            m->addOr(NEW_ID,generate,incoming,combined);sum=combined;
        }
        Bit next(m->addWire(NEW_ID));m->addAnd(NEW_ID,arith,sum,next);carry=next;
    }
    Sig data=plan.data;
    for (int slot=0;slot<3;slot++) data.replace(3*slot,result);
    plan.root->setPort(ID::B,data);
    log("PMUX_CARRY3 module=%s cell=%s width=3 operations=3 selection_proved=1\n",log_id(m),log_id(plan.root));
}

                                                                              
                                                                               
                                                                                 
inline bool matched_binary_signs(Cell *cell)
{
    const auto &a = cell->getParam(ID::A_SIGNED), &b = cell->getParam(ID::B_SIGNED);
    if (a.empty() || b.empty() || !a.is_fully_def() || !b.is_fully_def()) return false;
    auto av = a.try_as_int(false), bv = b.try_as_int(false);
    return av && bv && (*av == 0 || *av == 1) && *av == *bv;
}
}                                

inline int pmux_share_carry3(Yosys::RTLIL::Design *design)
{
    using namespace Yosys;
    using namespace pmux_resource_detail;
    int examined=0,changed=0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        std::vector<Cell *> roots;unsigned kinds=0;
        for (auto cell : module->selected_cells()) {
            if (cell->type==ID($pmux) && GetSize(cell->getPort(ID::Y))==3 && GetSize(cell->getPort(ID::S))==3)
                roots.push_back(cell);
            if (cell->type.in(ID($xor),ID($add),ID($sub)) && GetSize(cell->getPort(ID::Y))==3)
                kinds |= cell->type==ID($xor)?1:cell->type==ID($add)?2:4;
        }
        if (roots.empty() || kinds!=7) continue;
        Context ctx(module);if(!ctx.complete)continue;
                                                                             
                                                                             
        for (auto root : roots) {
            if (examined++>=64 || changed>=16) return changed;
            pmux_carry3_detail::Plan plan;
            if (!pmux_carry3_detail::make_plan(ctx,root,plan)) continue;
            pmux_carry3_detail::emit(ctx,plan);changed++;
        }
    }
    return changed;
}
#endif
