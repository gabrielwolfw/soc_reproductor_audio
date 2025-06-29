	soc_system u0 (
		.audio_BCLK            (<connected-to-audio_BCLK>),            //          audio.BCLK
		.audio_DACDAT          (<connected-to-audio_DACDAT>),          //               .DACDAT
		.audio_DACLRCK         (<connected-to-audio_DACLRCK>),         //               .DACLRCK
		.audio_config_SDAT     (<connected-to-audio_config_SDAT>),     //   audio_config.SDAT
		.audio_config_SCLK     (<connected-to-audio_config_SCLK>),     //               .SCLK
		.audio_xclkx_clk       (<connected-to-audio_xclkx_clk>),       //    audio_xclkx.clk
		.buttons_export        (<connected-to-buttons_export>),        //        buttons.export
		.clk_clk               (<connected-to-clk_clk>),               //            clk.clk
		.reset_reset_n         (<connected-to-reset_reset_n>),         //          reset.reset_n
		.seven_segments_export (<connected-to-seven_segments_export>), // seven_segments.export
		.memory_mem_a          (<connected-to-memory_mem_a>),          //         memory.mem_a
		.memory_mem_ba         (<connected-to-memory_mem_ba>),         //               .mem_ba
		.memory_mem_ck         (<connected-to-memory_mem_ck>),         //               .mem_ck
		.memory_mem_ck_n       (<connected-to-memory_mem_ck_n>),       //               .mem_ck_n
		.memory_mem_cke        (<connected-to-memory_mem_cke>),        //               .mem_cke
		.memory_mem_cs_n       (<connected-to-memory_mem_cs_n>),       //               .mem_cs_n
		.memory_mem_ras_n      (<connected-to-memory_mem_ras_n>),      //               .mem_ras_n
		.memory_mem_cas_n      (<connected-to-memory_mem_cas_n>),      //               .mem_cas_n
		.memory_mem_we_n       (<connected-to-memory_mem_we_n>),       //               .mem_we_n
		.memory_mem_reset_n    (<connected-to-memory_mem_reset_n>),    //               .mem_reset_n
		.memory_mem_dq         (<connected-to-memory_mem_dq>),         //               .mem_dq
		.memory_mem_dqs        (<connected-to-memory_mem_dqs>),        //               .mem_dqs
		.memory_mem_dqs_n      (<connected-to-memory_mem_dqs_n>),      //               .mem_dqs_n
		.memory_mem_odt        (<connected-to-memory_mem_odt>),        //               .mem_odt
		.memory_mem_dm         (<connected-to-memory_mem_dm>),         //               .mem_dm
		.memory_oct_rzqin      (<connected-to-memory_oct_rzqin>)       //               .oct_rzqin
	);

