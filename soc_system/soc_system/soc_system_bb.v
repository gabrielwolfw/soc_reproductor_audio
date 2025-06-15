
module soc_system (
	audio_BCLK,
	audio_DACDAT,
	audio_DACLRCK,
	audio_config_SDAT,
	audio_config_SCLK,
	audio_xclkx_clk,
	buttons_export,
	clk_clk,
	reset_reset_n,
	seven_segments_export,
	memory_mem_a,
	memory_mem_ba,
	memory_mem_ck,
	memory_mem_ck_n,
	memory_mem_cke,
	memory_mem_cs_n,
	memory_mem_ras_n,
	memory_mem_cas_n,
	memory_mem_we_n,
	memory_mem_reset_n,
	memory_mem_dq,
	memory_mem_dqs,
	memory_mem_dqs_n,
	memory_mem_odt,
	memory_mem_dm,
	memory_oct_rzqin);	

	input		audio_BCLK;
	output		audio_DACDAT;
	input		audio_DACLRCK;
	inout		audio_config_SDAT;
	output		audio_config_SCLK;
	output		audio_xclkx_clk;
	input	[2:0]	buttons_export;
	input		clk_clk;
	input		reset_reset_n;
	output	[27:0]	seven_segments_export;
	output	[12:0]	memory_mem_a;
	output	[2:0]	memory_mem_ba;
	output		memory_mem_ck;
	output		memory_mem_ck_n;
	output		memory_mem_cke;
	output		memory_mem_cs_n;
	output		memory_mem_ras_n;
	output		memory_mem_cas_n;
	output		memory_mem_we_n;
	output		memory_mem_reset_n;
	inout	[7:0]	memory_mem_dq;
	inout		memory_mem_dqs;
	inout		memory_mem_dqs_n;
	output		memory_mem_odt;
	output		memory_mem_dm;
	input		memory_oct_rzqin;
endmodule
