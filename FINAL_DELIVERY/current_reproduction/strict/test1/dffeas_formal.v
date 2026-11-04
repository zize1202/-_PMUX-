// Verification-only union of validated restricted domains. No synthesis use.
// Legacy: prn=1, aload/asdata/sclr/sload=0, dynamic clrn allowed.
// Extended: clrn=prn=1, aload=0; normal devclrn/devpor are implicitly one.
// Binary controls/data; explicit low/high initialization; no endpoint zinit.
// Existing vendor-library finite observations are supplemental, not formal proof.
module dffeas(output reg q, input d, clk, clrn, prn, ena,
              input asdata, aload, sclr, sload);
  parameter power_up = "low";
  parameter is_wysiwyg = "TRUE";
  wire selected_d = sclr ? 1'b0 : (sload ? asdata : d);
  initial q = (power_up == "high") ? 1'b1 : 1'b0;
  always @(posedge clk or negedge clrn)
    if (!clrn) q <= 1'b0;
    else if (ena) q <= selected_d;
endmodule
