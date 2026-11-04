// Development smoke input only. This is not one of the nine official cases.
module native_smoke(
    input [2:0] sel,
    input [15:0] a, b, c, d,
    output reg [15:0] y,
    output [3:0] parity
);
always @* begin
    case (sel)
        3'd0: y = a;
        3'd1: y = b;
        3'd2: y = c;
        3'd3: y = d;
        3'd4: y = a ^ b;
        3'd5: y = c & d;
        3'd6: y = {a[7:0],b[15:8]};
        default: y = 16'h739A;
    endcase
end
assign parity = {^y[15:12], ^y[11:8], ^y[7:4], ^y[3:0]};
endmodule
