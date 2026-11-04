// SPDX-License-Identifier: MIT
#include "pmux_priority_grouping.h"
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "pmux_resource_share.h"
#include "pmux_share_cone.h"
#include "pmux_share_carry3.h"
#include "pmux_share_banks_v2.h"
#include "pmux_share_dag.h"
#include "pmux_init_zero.h"
#include "pmux_small_rom.h"
#include "pmux_branch_constants.h"
#include <map>
#include <set>
#include <tuple>
#include <algorithm>
USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN
using Bit = RTLIL::SigBit;
const Bit Z(RTLIL::State::S0), O(RTLIL::State::S1);
bool binary(const RTLIL::SigSpec &s) { for (auto b:s) if(!b.wire && b!=Z && b!=O) return false; return true; }
struct Net {
 RTLIL::Module *m; SigMap sm; std::map<Bit,RTLIL::Cell*> drivers;
 std::map<std::tuple<int,Bit,Bit,Bit>,Bit> cache;
 Net(RTLIL::Module *m,RTLIL::Design *d,bool mux_only=false):m(m),sm(m) {
  CellTypes ct; if(!mux_only)ct.setup(d);                                                   
  for(auto c:m->cells()) for(auto &p:c->connections()) if(mux_only?p.first==ID::Y:ct.cell_output(c->type,p.first))
   for(auto b:sm(p.second)) { if(drivers.count(b)) drivers[b]=nullptr; else drivers[b]=c; }
 }
 Bit op(int kind,Bit a,Bit b=Z,Bit s=Z) {
  if(kind==0) { if(a==Z||b==Z)return Z; if(a==O)return b; if(b==O||a==b)return a; }
  if(kind==1) { if(a==O||b==O)return O; if(a==Z)return b; if(b==Z||a==b)return a; }
  if(kind==2) { if(a==b)return a; if(s==Z)return a; if(s==O)return b; if(a==Z&&b==O)return s; }
  if(kind==3) { if(a==Z)return O; if(a==O)return Z; }
  if(kind<2 && b<a) std::swap(a,b);
  auto key=std::make_tuple(kind,a,b,s); auto it=cache.find(key); if(it!=cache.end())return it->second;
  Bit y(m->addWire(NEW_ID));
  if(kind==0)m->addAnd(NEW_ID,a,b,y); if(kind==1)m->addOr(NEW_ID,a,b,y);
  if(kind==2)m->addMux(NEW_ID,a,b,s,y); if(kind==3)m->addNot(NEW_ID,a,y);
  cache[key]=y; return y;
 }
 Bit tree(std::vector<Bit> values,const std::vector<Bit>& leaves) {
  for(auto select:leaves) {
   std::vector<Bit> next; next.reserve(values.size()/2);
   for(size_t i=0;i<values.size();i+=2)next.push_back(op(2,values[i],values[i+1],select));
   values.swap(next);
  }
  log_assert(values.size()==1); return values[0];
 }
};
                                                                                    
                                                                               
struct Control {
 Net &n; std::set<Bit> inputs; std::set<Bit> visited, active; std::vector<Bit> leaves;
 std::map<Bit,int> index; std::map<Bit,bool> memo; int address=0, budget=4096; bool valid=true;
 Control(Net& n):n(n){}
 bool supported(RTLIL::Cell*c) {
  if(!c || !c->hasPort(ID::Y) || GetSize(n.sm(c->getPort(ID::Y)))!=1)return false;
  if(c->type.in(ID($and),ID($or),ID($not))) {
   if(GetSize(c->getPort(ID::A))!=1)return false;
   if(c->hasPort(ID::B)&&GetSize(c->getPort(ID::B))!=1)return false;
  }
  return c->type.in(ID($eq),ID($ne),ID($logic_not),ID($reduce_or),ID($reduce_and),ID($reduce_bool),ID($and),ID($or),ID($not),ID($logic_and),ID($logic_or));
 }
 void gather(Bit b,int depth=0) {
  if(active.count(b)){valid=false;return;}
  if(!valid || !visited.insert(b).second)return;
  if(--budget<0 || depth>24) {valid=false;return;}
  if(!b.wire){if(b!=Z&&b!=O)valid=false;return;}
  auto it=n.drivers.find(b); auto c=it==n.drivers.end()?nullptr:it->second;
  if(!supported(c)) {inputs.insert(b); if(inputs.size()>6)valid=false;return;}
  active.insert(b);
  for(auto p:{ID::A,ID::B}) if(c->hasPort(p))for(auto x:n.sm(c->getPort(p)))gather(x,depth+1);
  active.erase(b);
 }
 bool any(const RTLIL::SigSpec&s) {for(auto b:s)if(eval(b))return true;return false;}
 bool eval(Bit b) {
  if(!b.wire)return b==O;
  auto f=memo.find(b);if(f!=memo.end())return f->second;
  auto i=index.find(b);if(i!=index.end())return (address>>i->second)&1;
  auto c=n.drivers.at(b);auto a=n.sm(c->getPort(ID::A));
  auto bb=c->hasPort(ID::B)?n.sm(c->getPort(ID::B)):RTLIL::SigSpec(); bool v=false;
  if(c->type.in(ID($eq),ID($ne))) {
   v=true; bool sign=c->getParam(ID::A_SIGNED).as_bool()&&c->getParam(ID::B_SIGNED).as_bool();
   int w=std::max(GetSize(a),GetSize(bb));
   for(int j=0;j<w;j++) {
    Bit x=j<GetSize(a)?a[j]:(sign&&GetSize(a)?a[GetSize(a)-1]:Z);
    Bit y=j<GetSize(bb)?bb[j]:(sign&&GetSize(bb)?bb[GetSize(bb)-1]:Z);
    if(eval(x)!=eval(y)){v=false;break;}
   }
   if(c->type==ID($ne))v=!v;
  } else if(c->type.in(ID($logic_not),ID($not)))v=!any(a);
  else if(c->type==ID($reduce_and)) {v=true;for(auto x:a)v=v&&eval(x);}
  else if(c->type.in(ID($reduce_or),ID($reduce_bool)))v=any(a);
  else if(c->type.in(ID($and),ID($logic_and)))v=any(a)&&any(bb);
  else v=any(a)||any(bb);
  memo[b]=v;return v;
 }
 bool prepare(const RTLIL::SigSpec&s) {
  for(auto b:s)gather(b);if(!valid||inputs.empty())return false;
  leaves.assign(inputs.begin(),inputs.end());for(int i=0;i<GetSize(leaves);i++)index[leaves[i]]=i;return true;
 }
};
bool recover(Net &n,RTLIL::Cell *c) {
 auto a=n.sm(c->getPort(ID::A)),b=n.sm(c->getPort(ID::B)),s=n.sm(c->getPort(ID::S));
 int width=GetSize(a);if(width==0||width>1024||GetSize(s)>64)return false;
 Control ctl(n);if(!ctl.prepare(s))return false;
 int count=1<<GetSize(ctl.leaves);if(count*width>65536)return false;
                                                                                 
                                                                                
 if(GetSize(s)>7 && width>4)return false;
 std::vector<int> branches(count,-1);
 for(int v=0;v<count;v++) {
  ctl.address=v;ctl.memo.clear();
  for(int j=0;j<GetSize(s);j++)if(ctl.eval(s[j])) {if(branches[v]>=0)return false;branches[v]=j;}
 }
                                                                               
                                                                              
 RTLIL::SigSpec result;
 for(int j=0;j<width;j++) {
  std::vector<Bit> values;
  for(int v=0;v<count;v++) {
   Bit bit=branches[v]<0?a[j]:b[branches[v]*width+j];
   auto it=ctl.index.find(bit);if(it!=ctl.index.end())bit=((v>>it->second)&1)?O:Z;
   values.push_back(bit);
  }
  result.append(n.tree(values,ctl.leaves));
 }
 n.m->connect(c->getPort(ID::Y),result);return true;
}
bool preserved(RTLIL::Cell*c){return c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch"))||c->get_bool_attribute(ID(pmux_bank_local));}
int priorities(Net &n) {
 std::map<Bit,int> uses;std::set<Bit> nested;
 for(auto c:n.m->cells())for(auto &p:c->connections())if(p.first!=ID::Y&&p.first!=ID::Q)for(auto b:n.sm(p.second))uses[b]++;
 for(auto w:n.m->wires())if(w->port_output)for(auto b:n.sm(RTLIL::SigSpec(w)))uses[b]++;
 for(auto c:n.m->cells())if(c->type==ID($mux)&&!preserved(c))for(auto b:n.sm(c->getPort(ID::A)))nested.insert(b);
 std::vector<RTLIL::Cell*> remove;int changed=0;
 for(auto root:n.m->selected_cells()) {
  if(root->type!=ID($mux)||preserved(root))continue;
  auto y=n.sm(root->getPort(ID::Y));int w=GetSize(y);
  if(w<2||w>256||nested.count(y[0]))continue;
  std::vector<RTLIL::Cell*> chain;auto c=root;RTLIL::SigSpec tail;
  while(c && c->type==ID($mux)&&!preserved(c)&&GetSize(c->getPort(ID::Y))==w&&chain.size()<16) {
   chain.push_back(c);tail=n.sm(c->getPort(ID::A));auto found=n.drivers.find(tail[0]);
   auto next=found==n.drivers.end()?nullptr:found->second;bool ok=next&&next->type==ID($mux)&&n.sm(next->getPort(ID::Y))==tail;
   for(auto bit:tail)if(uses[bit]!=1)ok=false;
   c=ok?next:nullptr;
  }
  if(chain.size()<3||!binary(tail))continue;
  bool ok=true;for(auto cell:chain)if(!binary(n.sm(cell->getPort(ID::B)))||!binary(n.sm(cell->getPort(ID::S))))ok=false;
  if(!ok)continue;
  if(chain.size()==4 && tail==RTLIL::SigSpec(RTLIL::Const(RTLIL::State::S0,w))) {
   if(!pmux_priority_grouping_detail::eligible(n,chain,w,uses))continue;
   pmux_priority_grouping_detail::emit(n,chain,w);remove.push_back(root);changed++;continue;
  }
  Bit remaining=O;std::vector<Bit> masks;
  for(auto cell:chain){Bit s=n.sm(cell->getPort(ID::S))[0];masks.push_back(n.op(0,remaining,s));remaining=n.op(0,remaining,n.op(3,s));}
  RTLIL::SigSpec result;
  for(int j=0;j<w;j++) {
   Bit value=n.op(0,remaining,tail[j]);
   for(size_t i=0;i<chain.size();i++)value=n.op(1,value,n.op(0,masks[i],n.sm(chain[i]->getPort(ID::B))[j]));
   result.append(value);
  }
  n.m->connect(root->getPort(ID::Y),result);remove.push_back(root);changed++;
 }
 for(auto c:remove)n.m->remove(c);return changed;
}
#include "pmux_observable_decode.h"
#include "pmux_sdffe_boundary.h"
struct OptPmuxPass : Pass {
 OptPmuxPass():Pass("opt_pmux","exact bounded PMUX selector recovery and shared priority masks"){}
 void help()override{
  log("    opt_pmux -cycloneiv-sclr [selection]\nExplicit Cyclone IV target: use real hardware SCLR and eligible SLOAD at private PMUX\nsampling boundaries, only when all logical Q initial bits are known.\nUnknown init or unavailable primitive contract falls back to generic sdffce.\nThe target library must be loaded; no flag means generic RTLIL behavior.\n\n");
  log("    opt_pmux [selection]\nExact selector recovery (up to 6 control inputs), shared data trees and priority masks. No ABC calls.\n");
  log("Also consider up to 8 complete 32-word constant ROMs with 2..16 output bits\nfor exact bounded cofactor or residual sharing before technology mapping.\n");
  log("\n    opt_pmux -init-zero [selection]\nDeclare init=0 for source clocked FF bits without explicit init=0/1.\nPreserve explicit 0/1, including logical init=1 without inversion.\nRun immediately after proc, before sequential optimization or re-encoding.\nAlso fill unknown bits of writable synchronous RAMs using masked meminit\nentries; preserve explicit RAM initial values and address offsets.\nAll active writes must share one signal clock and polarity; pre-merged\nsynchronous read ports and wide/asynchronous writes are unsupported errors.\nLimits: 16M storage bits/RAM, 64M storage/init-scan bits/design, and\n65536 added initialization segments/design. Unsupported RAMs fail explicitly.\nROM contents, primary inputs, and unclocked latches remain unchanged.\nThis independent mode adds a source initial-state contract; it is not an\nunconstrained equivalence rewrite, and it does not optimize PMUX cells.\n");
 }
 void execute(std::vector<std::string> args,RTLIL::Design*d)override {
  if(args.size()>1 && args[1]=="-init-zero") {
   log_header(d,"Executing OPT_PMUX -init-zero pass (source logical FF initial-state contract).\n");
   extra_args(args,2,d);pmux_declare_zero_initial_state(d);return;
  }
  bool cycloneiv_sclr=args.size()>1&&args[1]=="-cycloneiv-sclr";
  log_header(d,"Executing OPT_PMUX pass (exact bounded selector recovery).\n");extra_args(args,cycloneiv_sclr?2:1,d);int seen=0,done=0,priority=0;
  int pmuxes=0,muxes=0,large_pmuxes=0,arithmetic=0,memory_reads=0,carry3_roots=0;unsigned carry3_kinds=0;
  for(auto m:d->selected_modules())if(!m->get_blackbox_attribute())for(auto c:m->selected_cells()) {
   if(c->type==ID($pmux)){pmuxes++;if(GetSize(c->getPort(ID::S))>=7)large_pmuxes++;if(GetSize(c->getPort(ID::Y))==3&&GetSize(c->getPort(ID::S))==3)carry3_roots++;}
   if(c->type==ID($mux))muxes++;
   if(c->type.in(ID($add),ID($sub),ID($xor))){arithmetic++;if(GetSize(c->getPort(ID::Y))==3)carry3_kinds|=c->type==ID($xor)?1:c->type==ID($add)?2:4;}
   if(c->type.in(ID($memrd),ID($memrd_v2),ID($mem),ID($mem_v2)))memory_reads++;
  }
  int roms=memory_reads?pmux_share_small_roms(d):0;
  if(!pmuxes && muxes<3){log("OPT_PMUX: shared %d small ROMs; no further eligible selection structure.\n",roms);return;}
  if(carry3_roots && carry3_kinds==7)pmux_share_carry3(d);
  int cones=(pmuxes>=2 && arithmetic>=3)?pmux_share_cones(d):0;
  int banks=large_pmuxes>=2?pmux_share_banks_v2(d):0;
  int resources=(pmuxes && arithmetic>=2)?pmux_share_resources(d):0;
  int dags=pmuxes>=2?pmux_share_dags(d):0;
  int post_dag_cones=dags?pmux_share_cones(d,true):0;
  int branch_constants=pmuxes?pmux_propagate_branch_constants(d):0;
  int sampling_boundaries=pmuxes?pmux_restate_sdffe_boundaries(d,cycloneiv_sclr):0;
  for(auto m:d->selected_modules())if(!m->get_blackbox_attribute()) {
   bool has_pmux=false,mux_only=true;int module_muxes=0;
   for(auto c:m->cells()){if(c->type==ID($mux))module_muxes++;else mux_only=false;if(c->type==ID($pmux)&&m->selected(c))has_pmux=true;}
   if(module_muxes>=3){ Net original(m,d,mux_only);priority+=priorities(original); }
                                                                            
                                                                           
   if(!has_pmux)continue;
   Net n(m,d);std::vector<RTLIL::Cell*> remove;
   for(auto c:m->selected_cells())if(c->type==ID($pmux)&&!preserved(c)){seen++;if(recover(n,c)){remove.push_back(c);done++;}}
   for(auto c:remove)m->remove(c);
  }
  log("OPT_PMUX: recovered %d/%d PMUX cells; shared %d priority chains.\n",done,seen,priority);
  log("OPT_PMUX: resource sharing rewrites %d.\n",resources);
  log("OPT_PMUX: shared %d arithmetic cones and %d output banks.\n",cones,banks);
  log("OPT_PMUX: flattened %d shared selection DAGs.\n",dags);
  log("OPT_PMUX: shared %d arithmetic cones after DAG flattening.\n",post_dag_cones);
  log("OPT_PMUX: shared %d small ROMs.\n",roms);
  log("OPT_PMUX: propagated branch constants in %d PMUX cells.\n",branch_constants);
  log("OPT_PMUX: restated %d private PMUX sampling boundaries.\n",sampling_boundaries);
  if(pmuxes>=3)pmux_observable_decode(d);
 }
} OptPmuxPass;
PRIVATE_NAMESPACE_END
