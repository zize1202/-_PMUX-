// Restricted behavioral counterpart, independently spelled as nested control.
// All pins/parameters must satisfy endpoint_sload_runner.validate_raw.
// ENA > SCLR > SLOAD(ASDATA) > D, binary contract, normal device globals one.
module dffeas(output reg q, input d, clk, clrn, prn, ena,
              input asdata, aload, sclr, sload);
  parameter power_up = "low";
  parameter is_wysiwyg = "TRUE";
  initial q = (power_up == "high") ? 1'b1 : 1'b0;
  always @(posedge clk or negedge clrn)
    if (!clrn) q <= 1'b0;
    else if (ena) begin
      if (sclr) q <= 1'b0;
      else if (sload) q <= asdata;
      else q <= d;
    end
endmodule
