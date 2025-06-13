	soc_system u0 (
		.buttons_export        (<connected-to-buttons_export>),        //        buttons.export
		.clk_clk               (<connected-to-clk_clk>),               //            clk.clk
		.reset_reset_n         (<connected-to-reset_reset_n>),         //          reset.reset_n
		.seven_segments_export (<connected-to-seven_segments_export>), // seven_segments.export
		.audio_xclkx_clk       (<connected-to-audio_xclkx_clk>),       //    audio_xclkx.clk
		.audio_config_SDAT     (<connected-to-audio_config_SDAT>),     //   audio_config.SDAT
		.audio_config_SCLK     (<connected-to-audio_config_SCLK>),     //               .SCLK
		.audio_BCLK            (<connected-to-audio_BCLK>),            //          audio.BCLK
		.audio_DACDAT          (<connected-to-audio_DACDAT>),          //               .DACDAT
		.audio_DACLRCK         (<connected-to-audio_DACLRCK>)          //               .DACLRCK
	);

