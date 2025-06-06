
module socaudio (
	audio_BCLK,
	audio_DACDAT,
	audio_DACLRCK,
	buttons_export,
	clk_clk,
	reset_reset_n,
	seven_segments_export);	

	input		audio_BCLK;
	output		audio_DACDAT;
	input		audio_DACLRCK;
	input	[3:0]	buttons_export;
	input		clk_clk;
	input		reset_reset_n;
	output	[27:0]	seven_segments_export;
endmodule
