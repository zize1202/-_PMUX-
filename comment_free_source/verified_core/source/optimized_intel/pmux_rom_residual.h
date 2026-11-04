// SPDX-License-Identifier: MIT
// Optional bounded replacement for an ALREADY accepted pmux_small_rom Plan.
// Include after pmux_small_rom.h. Reuses every original structural guard.
#ifndef PMUX_ROM_RESIDUAL_H
#define PMUX_ROM_RESIDUAL_H
#include <string>
namespace pmux_rom_residual_detail {
using namespace Yosys;
using namespace pmux_small_rom_detail;

                                                                               
                                                                               
struct AigCost {
    std::map<std::pair<int,int>,int> nodes;
    int band(int a,int b) {
        if (!a || !b || a==(b^1)) return 0;
        if (a==1) return b;
        if (b==1 || a==b) return a;
        if (b<a) std::swap(a,b);
        auto key=std::make_pair(a,b); auto it=nodes.find(key);
        if (it!=nodes.end()) return it->second;
        int ref=2*(6+int(nodes.size())); nodes[key]=ref; return ref;
    }
    int bor(int a,int b) { return band(a^1,b^1)^1; }
    int bxor(int a,int b) { return bor(band(a,b^1),band(a^1,b)); }
    int mux(int s,int a,int b) { return bor(band(s^1,a),band(s,b)); }
    int reduce(std::vector<int> refs,bool is_and) {
        if (refs.empty()) return is_and?1:0;
        while (refs.size()>1) {
            std::vector<int> next;
            for (int i=0;i<int(refs.size());i+=2)
                next.push_back(i+1==int(refs.size())?refs[i]:
                    is_and?band(refs[i],refs[i+1]):bor(refs[i],refs[i+1]));
            refs=next;
        }
        return refs[0];
    }
};
inline int cofactor_cost(const Plan &plan)
{
    AigCost aig; std::map<Key,int> leaves;
    for (auto key:plan.leaves) {
        auto vars=variables(key.first); std::vector<int> refs;
        for (int a=0;a<(1<<int(vars.size()));a++) refs.push_back((key.second>>a)&1u);
        for (auto variable:vars) {
            std::vector<int> next;
            for (int a=0;a<int(refs.size());a+=2) next.push_back(aig.mux(2*(variable+1),refs[a],refs[a+1]));
            refs=next;
        }
        leaves[key]=refs[0];
    }
    auto reference=[&](const Ref &ref) {
        if (ref.kind==0) return ref.value;
        return (ref.kind==1?2*(ref.value+1):leaves.at(ref.key))^int(ref.invert);
    };
    for (int b=0;b<int(plan.truth.size());b++) {
        const Choice &c=plan.candidates[b][plan.selected[b]];
        aig.mux(2*(c.split+1),reference(c.a),reference(c.b));
    }
    return int(aig.nodes.size());
}
struct ResidualPlan {
    enum Kind { EQ, NOT, OR, XOR };
    struct Op { Kind kind; int a,b; uint32_t truth; int aig; };
                                                                          
    std::vector<Op> ops;
    std::map<std::tuple<int,int,int>,int> cache;
    std::vector<int> baseline,output;
    std::map<unsigned,std::vector<int>> groups;
    AigCost aig;
    int old_cost=0,new_cost=0;
    static uint32_t address_truth(int bit) {
        uint32_t result=0;
        for (int a=0;a<32;a++) result|=uint32_t((a>>bit)&1)<<a;
        return result;
    }
    uint32_t truth(int ref) const {
        if (ref<2) return ref?0xffffffffu:0;
        if (ref<7) return address_truth(ref-2);
        return ops.at(ref-7).truth;
    }
    int aig_ref(int ref) const {
        if (ref<2) return ref;
        if (ref<7) return 2*(ref-1);
        return ops.at(ref-7).aig;
    }
    static std::string key(int ref) {
        if (ref<2) return "1'"+std::to_string(ref);
        if (ref<7) return "\\address["+std::to_string(ref-2)+"]";
        return "$residual$"+std::to_string(ref-7);
    }
    int op(Kind kind,int a,int b=-1) {
        if ((kind==OR || kind==XOR) && key(b)<key(a)) std::swap(a,b);
        auto k=std::make_tuple(int(kind),a,b); auto found=cache.find(k);
        if (found!=cache.end()) return found->second;
        uint32_t table=0; int ar=0;
        if (kind==EQ) {
            table=uint32_t(1)<<a; std::vector<int> literals;
            for (int i=0;i<5;i++) literals.push_back(2*(i+1)^int(!((a>>i)&1)));
            ar=aig.reduce(literals,true);
        } else if (kind==NOT) { table=~truth(a); ar=aig_ref(a)^1; }
        else if (kind==OR) { table=truth(a)|truth(b); ar=aig.bor(aig_ref(a),aig_ref(b)); }
        else { table=truth(a)^truth(b); ar=aig.bxor(aig_ref(a),aig_ref(b)); }
        int ref=7+int(ops.size()); ops.push_back({kind,a,b,table,ar}); cache[k]=ref; return ref;
    }
    int bor(int a,int b) {
        if (a==0) return b;
        if (b==0 || a==b) return a;
        return op(OR,a,b);
    }
    int reduce_or(std::vector<int> refs) {
        if (refs.empty()) return 0;
        while (refs.size()>1) {
            std::vector<int> next;
            for (int i=0;i<int(refs.size());i+=2)
                next.push_back(i+1==int(refs.size())?refs[i]:bor(refs[i],refs[i+1]));
            refs=next;
        }
        return refs[0];
    }
};
inline int bit_count(uint32_t x) { int n=0; while (x) { x&=x-1; n++; } return n; }
inline bool prepare_residual(const Plan &base,ResidualPlan &plan)
{
                                                                          
                                                                                 
                                                                
    for (auto table:base.truth) {
        int best=0,score=bit_count(table);
        for (int choice=1;choice<12;choice++) {
            uint32_t t=choice==1?0xffffffffu:
                ResidualPlan::address_truth((choice-2)%5)^(choice>=7?0xffffffffu:0u);
            int distance=bit_count(table^t);
            if (distance<score) { score=distance; best=choice; }
        }
        plan.baseline.push_back(best);
    }
    for (int a=0;a<32;a++) {
        unsigned delta=0;
        for (int b=0;b<int(base.truth.size());b++) {
            int choice=plan.baseline[b];
            int value=choice<2?choice:((a>>((choice-2)%5))&1)^int(choice>=7);
            if (value!=int((base.truth[b]>>a)&1u)) delta|=1u<<b;
        }
        if (delta) plan.groups[delta].push_back(a);
    }
                                                                         
    int nonzero_rows=0; for (auto &group:plan.groups) nonzero_rows+=int(group.second.size());
    if (plan.groups.empty() || int(plan.groups.size())>=nonzero_rows) return false;
    std::vector<std::pair<unsigned,int>> predicates;
    for (auto &group:plan.groups) {
        std::vector<int> comparisons;
        for (int a:group.second) comparisons.push_back(plan.op(ResidualPlan::EQ,a));
        predicates.push_back({group.first,plan.reduce_or(comparisons)});
    }
    for (int b=0;b<int(base.truth.size());b++) {
        int choice=plan.baseline[b];
        int baseline=choice<2?choice:2+(choice-2)%5;
        if (choice>=7) baseline=plan.op(ResidualPlan::NOT,baseline);
        std::vector<int> refs;
        for (auto pred:predicates) if ((pred.first>>b)&1u) refs.push_back(pred.second);
        std::sort(refs.begin(),refs.end(),[](int a,int b){ return ResidualPlan::key(a)<ResidualPlan::key(b); });
        int delta=plan.reduce_or(refs),out=delta;
        if (baseline==1) out=plan.op(ResidualPlan::NOT,delta);
        else if (baseline!=0) out=plan.op(ResidualPlan::XOR,baseline,delta);
        if (plan.truth(out)!=base.truth[b]) return false;                    
        plan.output.push_back(out);
    }
    plan.old_cost=cofactor_cost(base); plan.new_cost=int(plan.aig.nodes.size());
                                                                          
                                                                             
    int margin=std::max(4,(plan.old_cost+7)/8);
    return plan.new_cost+margin<=plan.old_cost;
}
inline void emit_residual(Mem &mem,const Plan &base,const ResidualPlan &plan)
{
    Module *module=mem.module;
    std::vector<SigBit> refs={State::S0,State::S1};
    for (auto bit:base.address) refs.push_back(bit);
    for (auto &op:plan.ops) {
        SigBit y(module->addWire(NEW_ID));
        if (op.kind==ResidualPlan::EQ) module->addEq(NEW_ID,base.address,Const(op.a,5),y);
        else if (op.kind==ResidualPlan::NOT) module->addNot(NEW_ID,refs.at(op.a),y);
        else if (op.kind==ResidualPlan::OR) module->addOr(NEW_ID,refs.at(op.a),refs.at(op.b),y);
        else module->addXor(NEW_ID,refs.at(op.a),refs.at(op.b),y);
        refs.push_back(y);
    }
    for (int b=0;b<int(plan.output.size());b++) module->connect(base.output[b],refs.at(plan.output[b]));
    mem.remove();
}
}                                      
#endif
