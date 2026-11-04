// Verification-only inverse of Yosys intel/common/ff_map.v.
// Accepted only after run_equivalence.py checks the tied control pins.
// This is not a general Altera simulation model or a synthesis optimization.
module dffeas(output reg q, input d, clk, clrn, prn, ena,
              input asdata, aload, sclr, sload);
  parameter power_up = "low";
  parameter is_wysiwyg = "TRUE";
  initial q = (power_up == "high") ? 1'b1 : 1'b0;
  always @(posedge clk or negedge clrn)
    if (!clrn) q <= 1'b0;
    else if (ena) q <= d;
endmodule
