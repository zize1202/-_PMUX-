// SPDX-License-Identifier: MIT
// Four-arm, zero-default exact priority factorization. No one-hot assumptions.
#ifndef PMUX_PRIORITY_GROUPING_H
#define PMUX_PRIORITY_GROUPING_H
#include "kernel/yosys.h"
#include <map>
#include <set>
#include <vector>

namespace pmux_priority_grouping_detail {
using namespace Yosys;
using Sig=RTLIL::SigSpec;
using Bit=RTLIL::SigBit;
using Cell=RTLIL::Cell;
inline bool protected_object(const RTLIL::AttrObject &o)
{
    return o.get_bool_attribute(ID::keep) || o.get_bool_attribute(ID(dont_touch));
}

template<class NetType>
bool eligible(NetType &n,const std::vector<Cell *> &chain,int width,
              const std::map<Bit,int> &uses)
{
    if (chain.size()!=4 || width<2 || width>256 || protected_object(*n.m)) return false;
    std::set<Bit> protected_bits,input_bits,outputs;
    // Only reached for a complete four-arm zero-default chain. Reuse the
    // caller's existing canonical driver/use tables; no extra Context/Net.
    for (auto wire:n.m->wires()) {
        bool protect=protected_object(*wire);
        if (!protect && !wire->port_input) continue;
        for (auto bit:n.sm(wire)) {
            if (protect) protected_bits.insert(bit);
            if (wire->port_input) input_bits.insert(bit);
        }
    }
    for (int i=0;i<4;i++) {
        Cell *cell=chain[i];
        if (!n.m->selected(cell) || cell->type!=ID($mux) || protected_object(*cell) ||
            cell->get_bool_attribute(ID(pmux_bank_local)) || cell->getParam(ID::WIDTH).as_int()!=width)
            return false;
        Sig y=n.sm(cell->getPort(ID::Y)),a=n.sm(cell->getPort(ID::A)),b=n.sm(cell->getPort(ID::B)),s=n.sm(cell->getPort(ID::S));
        if (GetSize(y)!=width || GetSize(a)!=width || GetSize(b)!=width || GetSize(s)!=1) return false;
        for (auto bit:y) {
            auto found=n.drivers.find(bit);
            if (!bit.wire || input_bits.count(bit) || protected_bits.count(bit) ||
                !outputs.insert(bit).second || found==n.drivers.end() || found->second!=cell) return false;
            if (i>0) { auto user=uses.find(bit);if(user==uses.end() || user->second!=1)return false; }
        }
        if (i<3 && a!=n.sm(chain[i+1]->getPort(ID::Y))) return false;
        if (i==3 && a!=Sig(RTLIL::Const(State::S0,width))) return false;
    }
    for (auto cell:chain) for (auto port:{ID::B,ID::S}) for (auto bit:n.sm(cell->getPort(port))) {
        if (!bit.wire) { if(bit!=Bit(State::S0)&&bit!=Bit(State::S1))return false;continue; }
        auto found=n.drivers.find(bit);
        if (protected_bits.count(bit) || outputs.count(bit) ||
            (found!=n.drivers.end() && (!found->second || input_bits.count(bit)))) return false;
    }
    return true;
}

template<class NetType>
void emit(NetType &n,const std::vector<Cell *> &chain,int width)
{
    auto m=n.m;
    std::vector<Sig> data;std::vector<Bit> select;
    for (auto cell:chain) {data.push_back(n.sm(cell->getPort(ID::B)));select.push_back(n.sm(cell->getPort(ID::S))[0]);}
    Sig masked_b(m->addWire(NEW_ID,width));
    m->addAnd(NEW_ID,Sig(select[1]).repeat(width),data[1],masked_b);
    Sig high(m->addWire(NEW_ID,width));m->addMux(NEW_ID,masked_b,data[0],select[0],high);
    Sig masked_d(m->addWire(NEW_ID,width));
    m->addAnd(NEW_ID,Sig(select[3]).repeat(width),data[3],masked_d);
    Sig low(m->addWire(NEW_ID,width));m->addMux(NEW_ID,masked_d,data[2],select[2],low);
    Bit not_a(m->addWire(NEW_ID));m->addNot(NEW_ID,select[0],not_a);
    Bit not_b(m->addWire(NEW_ID));m->addNot(NEW_ID,select[1],not_b);
    Bit enable(m->addWire(NEW_ID));m->addAnd(NEW_ID,not_a,not_b,enable);
    Sig masked_low(m->addWire(NEW_ID,width));m->addAnd(NEW_ID,Sig(enable).repeat(width),low,masked_low);
    m->addOr(NEW_ID,high,masked_low,chain.front()->getPort(ID::Y));
    log("PMUX_PRIORITY_GROUP4 module=%s cell=%s width=%d zero_default=1 exact_priority=1\n",log_id(m),log_id(chain.front()),width);
}
} // namespace pmux_priority_grouping_detail
#endif
