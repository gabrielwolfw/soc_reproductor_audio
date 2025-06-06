	component socaudio is
		port (
			audio_BCLK            : in  std_logic                     := 'X';             -- BCLK
			audio_DACDAT          : out std_logic;                                        -- DACDAT
			audio_DACLRCK         : in  std_logic                     := 'X';             -- DACLRCK
			buttons_export        : in  std_logic_vector(3 downto 0)  := (others => 'X'); -- export
			clk_clk               : in  std_logic                     := 'X';             -- clk
			reset_reset_n         : in  std_logic                     := 'X';             -- reset_n
			seven_segments_export : out std_logic_vector(27 downto 0)                     -- export
		);
	end component socaudio;

	u0 : component socaudio
		port map (
			audio_BCLK            => CONNECTED_TO_audio_BCLK,            --          audio.BCLK
			audio_DACDAT          => CONNECTED_TO_audio_DACDAT,          --               .DACDAT
			audio_DACLRCK         => CONNECTED_TO_audio_DACLRCK,         --               .DACLRCK
			buttons_export        => CONNECTED_TO_buttons_export,        --        buttons.export
			clk_clk               => CONNECTED_TO_clk_clk,               --            clk.clk
			reset_reset_n         => CONNECTED_TO_reset_reset_n,         --          reset.reset_n
			seven_segments_export => CONNECTED_TO_seven_segments_export  -- seven_segments.export
		);

