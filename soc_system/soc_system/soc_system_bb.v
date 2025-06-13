
module soc_system (
	buttons_export,
	clk_clk,
	reset_reset_n,
	seven_segments_export,
	audio_xclkx_clk,
	audio_config_SDAT,
	audio_config_SCLK,
	audio_BCLK,
	audio_DACDAT,
	audio_DACLRCK);	

	input	[2:0]	buttons_export;
	input		clk_clk;
	input		reset_reset_n;
	output	[27:0]	seven_segments_export;
	output		audio_xclkx_clk;
	inout		audio_config_SDAT;
	output		audio_config_SCLK;
	input		audio_BCLK;
	output		audio_DACDAT;
	input		audio_DACLRCK;
endmodule
