// Verification-only union of validated restricted domains. No synthesis use.
                                                                  
                                                                           
                                                                             
                                                                                  
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
