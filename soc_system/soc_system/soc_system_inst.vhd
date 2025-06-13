	component soc_system is
		port (
			buttons_export        : in    std_logic_vector(2 downto 0)  := (others => 'X'); -- export
			clk_clk               : in    std_logic                     := 'X';             -- clk
			reset_reset_n         : in    std_logic                     := 'X';             -- reset_n
			seven_segments_export : out   std_logic_vector(27 downto 0);                    -- export
			audio_xclkx_clk       : out   std_logic;                                        -- clk
			audio_config_SDAT     : inout std_logic                     := 'X';             -- SDAT
			audio_config_SCLK     : out   std_logic;                                        -- SCLK
			audio_BCLK            : in    std_logic                     := 'X';             -- BCLK
			audio_DACDAT          : out   std_logic;                                        -- DACDAT
			audio_DACLRCK         : in    std_logic                     := 'X'              -- DACLRCK
		);
	end component soc_system;

	u0 : component soc_system
		port map (
			buttons_export        => CONNECTED_TO_buttons_export,        --        buttons.export
			clk_clk               => CONNECTED_TO_clk_clk,               --            clk.clk
			reset_reset_n         => CONNECTED_TO_reset_reset_n,         --          reset.reset_n
			seven_segments_export => CONNECTED_TO_seven_segments_export, -- seven_segments.export
			audio_xclkx_clk       => CONNECTED_TO_audio_xclkx_clk,       --    audio_xclkx.clk
			audio_config_SDAT     => CONNECTED_TO_audio_config_SDAT,     --   audio_config.SDAT
			audio_config_SCLK     => CONNECTED_TO_audio_config_SCLK,     --               .SCLK
			audio_BCLK            => CONNECTED_TO_audio_BCLK,            --          audio.BCLK
			audio_DACDAT          => CONNECTED_TO_audio_DACDAT,          --               .DACDAT
			audio_DACLRCK         => CONNECTED_TO_audio_DACLRCK          --               .DACLRCK
		);

