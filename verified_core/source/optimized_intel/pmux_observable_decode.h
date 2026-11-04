// SPDX-License-Identifier: MIT
// Atomic observation-aware PMUX projection plus exact equality-bank factoring.
// The projection preserves sampled transitions over binary states/inputs, not
// standalone symmetric four-state PMUX values outside the proven enable domain.
// Equality factoring itself preserves 0/1/X/Z values on every old equality Y.
#ifndef PMUX_OBSERVABLE_DECODE_H
#define PMUX_OBSERVABLE_DECODE_H
namespace pmux_observable_detail {
using namespace Yosys;
using namespace pmux_resource_detail;

inline bool parameter(Cell *c, RTLIL::IdString id, int expected)
{
    if (!c->hasParam(id)) return false;
    const auto &v = c->getParam(id);
    if (v.empty() || !v.is_fully_def()) return false;
    auto n = v.try_as_int(false);
    return n && *n == expected;
}

struct Equality { Cell *cell = nullptr; Sig selector; Bit y; unsigned code = 0; };
struct Projection {
    Cell *cell = nullptr;
    Sig y, selects;
    std::vector<unsigned> codes;
    std::vector<std::vector<int>> rows;
    std::vector<int> literals;
    std::vector<Cell *> readers;
};
struct Plan {
    Cell *seed = nullptr;
    Sig selector;
    std::vector<Equality> bank;
    std::vector<int> common, residual;
    std::vector<Projection> projections;
};

inline bool equality(Context &ctx, Bit bit, Equality &out)
{
    auto c = ctx.whole_driver(Sig(bit));
    if (!c || c->type != ID($eq) || !ctx.module->selected(c) || protected_object(c) ||
        ctx.protected_bits.count(bit) || c->connections().size() != 3 || !c->hasPort(ID::A) || !c->hasPort(ID::B) ||
        !parameter(c,ID::A_SIGNED,0) || !parameter(c,ID::B_SIGNED,0) || !parameter(c,ID::Y_WIDTH,1)) return false;
    Sig a = ctx.sigmap(c->getPort(ID::A)), b = ctx.sigmap(c->getPort(ID::B));
    int w = GetSize(a);
    if (w < 5 || w > 8 || GetSize(b) != w || !parameter(c,ID::A_WIDTH,w) || !parameter(c,ID::B_WIDTH,w)) return false;
    if (a.is_fully_const()) std::swap(a,b);
    if (!b.is_fully_const() || !known_literals(b) || ctx.protected_signal(a)) return false;
    pool<Bit> unique;
    for (auto x : a)
        if (!x.wire || x == bit || ctx.multiple.count(x) || !unique.insert(x).second ||
            (ctx.input_bits.count(x) && ctx.driver.count(x))) return false;
    out.cell = c; out.selector = a; out.y = bit;
    for (int i = 0; i < w; i++) if (b[i] == Bit(State::S1)) out.code |= 1u << i;
    return true;
}

// 0, 1, then all positive selector bits in actual LSB-first SigSpec order,
// then all negative selector bits. No arbitrary data/care approximation.
inline int value(int literal, unsigned code, int width)
{
    if (literal < 2) return literal;
    int p = literal - 2;
    return int((code >> (p % width)) & 1u) ^ int(p >= width);
}

inline int data_literal(Context &ctx, Bit bit, const Sig &selector)
{
    if (bit == Bit(State::S0)) return 0;
    if (bit == Bit(State::S1)) return 1;
    for (int i = 0; i < GetSize(selector); i++) if (bit == selector[i]) return i+2;
    auto c = ctx.whole_driver(Sig(bit));
    if (!c || c->type != ID($not) || !c->hasPort(ID::A) || protected_object(c) || ctx.protected_bits.count(bit) ||
        !parameter(c,ID::A_WIDTH,1) || !parameter(c,ID::Y_WIDTH,1)) return -1;
    Sig a = ctx.sigmap(c->getPort(ID::A));
    if (GetSize(a) != 1 || ctx.protected_signal(a)) return -1;
    for (int i = 0; i < GetSize(selector); i++) if (a[0] == selector[i]) return 2+GetSize(selector)+i;
    return -1;
}

inline bool exact_active_factor(Context &ctx, Bit bit, const Sig &selects, int depth, int &budget)
{
    if (depth > 8 || --budget < 0) return false;
    auto c = ctx.whole_driver(Sig(bit));
    if (!c || !c->hasPort(ID::A) || protected_object(c) || ctx.protected_bits.count(bit) || !parameter(c,ID::Y_WIDTH,1)) return false;
    Sig a = ctx.sigmap(c->getPort(ID::A)), terms;
    if (!parameter(c,ID::A_WIDTH,GetSize(a))) return false;
    if (c->type.in(ID($reduce_or),ID($reduce_bool))) {
        if (GetSize(a) != GetSize(selects)) return false;
        std::set<Bit> left(a.begin(),a.end()), right(selects.begin(),selects.end());
        return left.size() == size_t(GetSize(a)) && left == right;
    }
    if (c->type == ID($reduce_and)) {
        if (GetSize(a) < 1 || GetSize(a) > 16) return false;
        terms = a;
    } else if (c->type.in(ID($and),ID($logic_and))) {
        if (!c->hasPort(ID::B)) return false;
        Sig b = ctx.sigmap(c->getPort(ID::B));
        if (GetSize(a) != 1 || GetSize(b) != 1 || !parameter(c,ID::B_WIDTH,1)) return false;
        terms = a; terms.append(b);
    } else return false;
    for (auto child : terms) if (exact_active_factor(ctx,child,selects,depth+1,budget)) return true;
    return false;
}

// Explicit bounded combinational fan-in audit. Unknown/undriven objects and
// unsupported combinational cells are rejected, never promoted to free inputs.
struct Cone {
    Context &ctx;
    pool<Bit> forbidden, done, active;
    int budget = 512;
    Cone(Context &ctx, const pool<Bit> &forbidden) : ctx(ctx), forbidden(forbidden) {}
    bool bit(Bit x, int depth=0) {
        if (depth > 24 || --budget < 0 || forbidden.count(x) || ctx.protected_bits.count(x) || ctx.multiple.count(x)) return false;
        if (!x.wire) return x == Bit(State::S0) || x == Bit(State::S1);
        if (done.count(x)) return true;
        if (active.count(x)) return false;
        if (ctx.input_bits.count(x)) return !ctx.driver.count(x);
        if (!ctx.driver.count(x)) return false;
        auto c = ctx.driver.at(x);
        if (protected_object(c)) return false;
        if (c->is_builtin_ff() && c->hasPort(ID::CLK) && c->hasPort(ID::Q)) {
            Sig q = ctx.sigmap(c->getPort(ID::Q));
            for (auto y : q) if (y == x) { done.insert(x); return true; }
            return false;
        }
        if (!c->type.in(ID($not),ID($and),ID($or),ID($xor),ID($xnor),ID($logic_not),ID($logic_and),ID($logic_or),
                        ID($reduce_and),ID($reduce_or),ID($reduce_bool),ID($eq),ID($ne),ID($mux),ID($pmux))) return false;
        active.insert(x);
        for (auto id : {ID::A,ID::B,ID::S}) if (c->hasPort(id))
            for (auto y : ctx.sigmap(c->getPort(id))) if (!bit(y,depth+1)) return false;
        active.erase(x); done.insert(x); return true;
    }
    bool signal(const Sig &s) { for (auto x : s) if (!bit(x)) return false; return true; }
};

inline bool projection(Context &ctx, Cell *c, const Plan &bank, Projection &p)
{
    if (c == bank.seed || c->type != ID($pmux) || !ctx.module->selected(c) || protected_object(c) ||
        !c->hasPort(ID::A) || !c->hasPort(ID::B) || !c->hasPort(ID::S) || !c->hasPort(ID::Y)) return false;
    Sig a = ctx.sigmap(c->getPort(ID::A)), b = ctx.sigmap(c->getPort(ID::B));
    Sig s = ctx.sigmap(c->getPort(ID::S)), y = ctx.sigmap(c->getPort(ID::Y));
    int w = GetSize(a), n = GetSize(s);
    if (w < 1 || w > 16 || n < 2 || n > 8 || GetSize(y) != w || GetSize(b) != w*n ||
        !parameter(c,ID::WIDTH,w) || !parameter(c,ID::S_WIDTH,n) || ctx.whole_driver(y) != c ||
        ctx.protected_signal(a) || ctx.protected_signal(b) || ctx.protected_signal(s) || ctx.protected_signal(y)) return false;
    for (auto x : a) if (x != Bit(State::Sx)) return false;
    pool<Bit> selected;
    for (auto x : s) {
        if (!selected.insert(x).second) return false;
        bool found = false;
        for (const auto &eq : bank.bank) if (x == eq.y) { p.codes.push_back(eq.code); found = true; break; }
        if (!found) return false;
    }
    pool<Cell *> readers;
    for (auto x : y) {
        if (ctx.output_bits.count(x) || ctx.input_bits.count(x) || !ctx.consumers.count(x)) return false;
        for (auto reader : ctx.consumers.at(x)) readers.insert(reader);
    }
    if (readers.empty() || readers.size() > 8) return false;
    for (auto ff : readers) {
        if (ff->type != ID($dffe) || !ff->hasPort(ID::D) || !ff->hasPort(ID::Q) || !ff->hasPort(ID::EN) || !ff->hasPort(ID::CLK) ||
            protected_object(ff) || !parameter(ff,ID::WIDTH,w) ||
            !parameter(ff,ID::EN_POLARITY,1) || (!parameter(ff,ID::CLK_POLARITY,0) && !parameter(ff,ID::CLK_POLARITY,1)) ||
            ctx.sigmap(ff->getPort(ID::D)) != y) return false;
        Sig clk = ctx.sigmap(ff->getPort(ID::CLK)), en = ctx.sigmap(ff->getPort(ID::EN)), q = ctx.sigmap(ff->getPort(ID::Q));
        if (GetSize(clk) != 1 || !clk[0].wire || GetSize(en) != 1 || GetSize(q) != w) return false;
        for (auto &port : ff->connections()) {
            Sig signal = ctx.sigmap(port.second);
            if (ctx.protected_signal(signal)) return false;
            if (port.first != ID::D) for (auto x : signal) for (auto py : y) if (x == py) return false;
        }
        int budget = 64;
        if (!exact_active_factor(ctx,en[0],s,0,budget)) return false;
        p.readers.push_back(ff);
    }
    for (int slot = 0; slot < n; slot++) {
        std::vector<int> row;
        for (int j = 0; j < w; j++) {
            int literal = data_literal(ctx,b[slot*w+j],bank.selector);
            if (literal < 0) return false;
            row.push_back(value(literal,p.codes[slot],GetSize(bank.selector)));
        }
        p.rows.push_back(row);
    }
    for (int j = 0; j < w; j++) {
        int selected_literal = -1;
        for (int candidate = 0; candidate < 2+2*GetSize(bank.selector); candidate++) {
            bool matches = true;
            for (int slot = 0; slot < n; slot++)
                if (value(candidate,p.codes[slot],GetSize(bank.selector)) != p.rows[slot][j]) matches = false;
            if (matches) { selected_literal = candidate; break; }
        }
        if (selected_literal < 0) return false;
        p.literals.push_back(selected_literal);
    }
    p.cell = c; p.y = y; p.selects = s; return true;
}

inline bool prepare(Context &ctx, Cell *seed, const std::vector<Cell *> &roots, Plan &plan)
{
    if (protected_object(seed) || !seed->hasPort(ID::S) || !seed->hasPort(ID::Y)) return false;
    Sig seed_y = ctx.sigmap(seed->getPort(ID::Y));
    if (ctx.whole_driver(seed_y) != seed || ctx.protected_signal(seed_y)) return false;
    Sig selects = ctx.sigmap(seed->getPort(ID::S));
    if (GetSize(selects) < 3 || GetSize(selects) > 8) return false;
    plan.seed = seed;
    std::set<unsigned> codes; pool<Bit> eq_outputs;
    for (auto bit : selects) {
        Equality eq;
        if (!equality(ctx,bit,eq) || !codes.insert(eq.code).second || !eq_outputs.insert(eq.y).second) return false;
        if (plan.selector.empty()) plan.selector = eq.selector;
        if (plan.selector != eq.selector) return false;
        plan.bank.push_back(eq);
    }
    std::sort(plan.bank.begin(),plan.bank.end(),[](const Equality &a,const Equality &b){return a.cell->name.str() < b.cell->name.str();});
    for (int i = 0; i < GetSize(plan.selector); i++) {
        bool same = true;
        for (const auto &eq : plan.bank) if (((eq.code >> i)&1u) != ((plan.bank.front().code >> i)&1u)) same = false;
        (same ? plan.common : plan.residual).push_back(i);
    }
    if (plan.common.size() < 2 || plan.common.size() > 4 || plan.residual.empty() || plan.residual.size() > 4) return false;
    for (auto c : roots) {
        Projection p;
        if (projection(ctx,c,plan,p)) plan.projections.push_back(p);
        if (plan.projections.size() > 8) return false;
    }
    if (plan.projections.size() < 2) return false;
    pool<Bit> care_outputs;
    for (const auto &p : plan.projections) for (auto bit : p.y)
        if (!care_outputs.insert(bit).second || eq_outputs.count(bit)) return false;
    pool<Bit> selector_forbidden = care_outputs;
    for (auto bit : eq_outputs) selector_forbidden.insert(bit);
    Cone selector_cone(ctx,selector_forbidden);
    if (!selector_cone.signal(plan.selector)) return false;
    Cone observer_cone(ctx,care_outputs);
    for (const auto &p : plan.projections) for (auto ff : p.readers)
        if (!observer_cone.signal(ctx.sigmap(ff->getPort(ID::EN))) || !observer_cone.signal(ctx.sigmap(ff->getPort(ID::CLK)))) return false;
    // Recheck every Boolean selector assignment; each code has at most one
    // branch and every observable bit equals its planned literal. For EN=0,
    // every DFFE holds arbitrary Q, so no Q enumeration or state assumption.
    for (unsigned code = 0; code < (1u << GetSize(plan.selector)); code++)
        for (const auto &p : plan.projections) {
            int active = -1;
            for (int i = 0; i < int(p.codes.size()); i++) if (p.codes[i] == code) { if (active >= 0) return false; active = i; }
            if (active < 0) continue;
            for (int j = 0; j < int(p.literals.size()); j++)
                if (value(p.literals[j],code,GetSize(plan.selector)) != p.rows[active][j]) return false;
        }
    return true;
}

inline void emit(Module *module, const Plan &plan)
{
    std::map<int,Bit> negative;
    for (const auto &p : plan.projections) {
        Sig replacement;
        for (auto literal : p.literals) {
            if (literal < 2) replacement.append(literal ? State::S1 : State::S0);
            else if (literal-2 < GetSize(plan.selector)) replacement.append(plan.selector[literal-2]);
            else {
                int position = literal-2-GetSize(plan.selector);
                if (!negative.count(position)) {
                    Bit n(module->addWire(NEW_ID)); module->addNot(NEW_ID,plan.selector[position],n); negative[position] = n;
                }
                replacement.append(negative.at(position));
            }
        }
        module->connect(p.y,replacement); module->remove(p.cell);
    }
    Sig common, residual; unsigned hcode = 0;
    for (int i = 0; i < int(plan.common.size()); i++) {
        int position = plan.common[i]; common.append(plan.selector[position]);
        hcode |= ((plan.bank.front().code >> position)&1u) << i;
    }
    for (auto position : plan.residual) residual.append(plan.selector[position]);
    Sig h(module->addWire(NEW_ID)); module->addEq(NEW_ID,common,Sig(RTLIL::Const(hcode,GetSize(common))),h,false);
    for (const auto &eq : plan.bank) {
        unsigned code = 0;
        for (int i = 0; i < int(plan.residual.size()); i++) code |= ((eq.code >> plan.residual[i])&1u) << i;
        Sig r(module->addWire(NEW_ID)); module->addEq(NEW_ID,residual,Sig(RTLIL::Const(code,GetSize(residual))),r,false);
        // Exact partitioned equality: a known mismatch gives 0; otherwise an
        // unknown bit yields X in either the full eq or H & residual eq.
        eq.cell->type = ID($and);
        eq.cell->setParam(ID::A_WIDTH,1); eq.cell->setParam(ID::B_WIDTH,1);
        eq.cell->setPort(ID::A,h); eq.cell->setPort(ID::B,r);
    }
    log("PMUX_OBSERVABLE_DECODE module=%s selector_bits=%d bank=%d common_bits=%d residual_bits=%d projected_pmuxes=%d\n",
        log_id(module),GetSize(plan.selector),int(plan.bank.size()),int(plan.common.size()),int(plan.residual.size()),int(plan.projections.size()));
}
} // namespace pmux_observable_detail

inline int pmux_observable_decode(Yosys::RTLIL::Design *design)
{
    using namespace pmux_observable_detail;
    int changed = 0, inspected = 0;
    for (auto module : design->selected_modules()) {
        if (module->get_blackbox_attribute() || protected_object(module)) continue;
        std::vector<Cell *> roots; int dffes = 0, wide_equalities = 0;
        for (auto c : module->cells()) {
            if (c->type == ID($pmux) && module->selected(c)) roots.push_back(c);
            if (c->type == ID($dffe)) dffes++;
            if (c->type == ID($eq) && c->hasPort(ID::A) && module->selected(c) && GetSize(c->getPort(ID::A)) > 4) wide_equalities++;
        }
        if (roots.size() < 3 || roots.size() > 64 || dffes < 2 || wide_equalities < 3) continue;
        Context ctx(module);
        if (!ctx.complete) continue;
        std::sort(roots.begin(),roots.end(),[](Cell *a,Cell *b){return a->name.str() < b->name.str();});
        for (auto seed : roots) {
            if (++inspected > 32 || changed >= 8) return changed;
            Plan plan;
            if (!prepare(ctx,seed,roots,plan)) continue;
            emit(module,plan); changed++;
            // Immutable Context may point to removed PMUX cells. Commit at
            // most one fully prepared bank per module, then stop using it.
            break;
        }
    }
    return changed;
}
#endif
