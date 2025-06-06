module top (
    input        clk_fpga,
    input        rst_n,
    input        audio_BCLK,
    output       audio_DACDAT,
    input        audio_DACLRCK,
    input  [3:0] buttons,
    output [27:0] seven_segments
);

    socaudio u_socaudio (
        .audio_BCLK(audio_BCLK),
        .audio_DACDAT(audio_DACDAT),
        .audio_DACLRCK(audio_DACLRCK),
        .buttons_export(buttons),
        .clk_clk(clk_fpga),
        .reset_reset_n(rst_n),
        .seven_segments_export(seven_segments)
    );

endmodule