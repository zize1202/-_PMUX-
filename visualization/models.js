(function(root){
'use strict';
const mask=w=>(2**w)-1;
function selector(address,data,overlap=false){const a=address&1,b=(address>>1)&1;const s=overlap?[!b,!a,!!a&&!b,!!a&&!!b]:[address===0,address===1,address===2,address===3];const on=s.map((v,i)=>v?i:-1).filter(i=>i>=0);return {controls:s.map(Number),selected:on,valid:!overlap,before:on.length===1?data[on[0]]:null,after:!overlap?data[address]:null};}
function priority(controls,data,w){const m=mask(w);let before=0;for(let i=3;i>=0;i--)before=controls[i]?data[i]&m:before;const high=controls[0]?data[0]&m:controls[1]?data[1]&m:0;const low=controls[2]?data[2]&m:controls[3]?data[3]&m:0;const gate=!controls[0]&&!controls[1];return{before,after:(high|(gate?low:0))&m,high,low,gate,selected:controls.findIndex(Boolean)};}
function share(address,data,x,w){const m=mask(w);const results=data.map(v=>(x+v)&m);return{before:results[address],after:(x+data[address])&m,results,selected:address};}
const api={mask,selector,priority,share};if(typeof module!=='undefined'&&module.exports)module.exports=api;else root.PMUX_MODELS=api;
})(typeof window!=='undefined'?window:globalThis);
