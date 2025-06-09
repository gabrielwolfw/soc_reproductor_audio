	component soc_audio is
		port (
			audio_BCLK                   : in    std_logic                     := 'X';             -- BCLK
			audio_DACDAT                 : out   std_logic;                                        -- DACDAT
			audio_DACLRCK                : in    std_logic                     := 'X';             -- DACLRCK
			buttons_export               : in    std_logic_vector(3 downto 0)  := (others => 'X'); -- export
			clk_clk                      : in    std_logic                     := 'X';             -- clk
			hps_io_hps_io_sdio_inst_CMD  : inout std_logic                     := 'X';             -- hps_io_sdio_inst_CMD
			hps_io_hps_io_sdio_inst_D0   : inout std_logic                     := 'X';             -- hps_io_sdio_inst_D0
			hps_io_hps_io_sdio_inst_D1   : inout std_logic                     := 'X';             -- hps_io_sdio_inst_D1
			hps_io_hps_io_sdio_inst_CLK  : out   std_logic;                                        -- hps_io_sdio_inst_CLK
			hps_io_hps_io_sdio_inst_D2   : inout std_logic                     := 'X';             -- hps_io_sdio_inst_D2
			hps_io_hps_io_sdio_inst_D3   : inout std_logic                     := 'X';             -- hps_io_sdio_inst_D3
			hps_io_hps_io_uart0_inst_RX  : in    std_logic                     := 'X';             -- hps_io_uart0_inst_RX
			hps_io_hps_io_uart0_inst_TX  : out   std_logic;                                        -- hps_io_uart0_inst_TX
			hps_io_hps_io_uart0_inst_CTS : in    std_logic                     := 'X';             -- hps_io_uart0_inst_CTS
			hps_io_hps_io_uart0_inst_RTS : out   std_logic;                                        -- hps_io_uart0_inst_RTS
			memory_mem_a                 : out   std_logic_vector(12 downto 0);                    -- mem_a
			memory_mem_ba                : out   std_logic_vector(2 downto 0);                     -- mem_ba
			memory_mem_ck                : out   std_logic;                                        -- mem_ck
			memory_mem_ck_n              : out   std_logic;                                        -- mem_ck_n
			memory_mem_cke               : out   std_logic;                                        -- mem_cke
			memory_mem_cs_n              : out   std_logic;                                        -- mem_cs_n
			memory_mem_ras_n             : out   std_logic;                                        -- mem_ras_n
			memory_mem_cas_n             : out   std_logic;                                        -- mem_cas_n
			memory_mem_we_n              : out   std_logic;                                        -- mem_we_n
			memory_mem_reset_n           : out   std_logic;                                        -- mem_reset_n
			memory_mem_dq                : inout std_logic_vector(31 downto 0) := (others => 'X'); -- mem_dq
			memory_mem_dqs               : inout std_logic_vector(3 downto 0)  := (others => 'X'); -- mem_dqs
			memory_mem_dqs_n             : inout std_logic_vector(3 downto 0)  := (others => 'X'); -- mem_dqs_n
			memory_mem_odt               : out   std_logic;                                        -- mem_odt
			memory_mem_dm                : out   std_logic_vector(3 downto 0);                     -- mem_dm
			memory_oct_rzqin             : in    std_logic                     := 'X';             -- oct_rzqin
			reset_reset_n                : in    std_logic                     := 'X';             -- reset_n
			seven_segments_export        : out   std_logic_vector(27 downto 0)                     -- export
		);
	end component soc_audio;

	u0 : component soc_audio
		port map (
			audio_BCLK                   => CONNECTED_TO_audio_BCLK,                   --          audio.BCLK
			audio_DACDAT                 => CONNECTED_TO_audio_DACDAT,                 --               .DACDAT
			audio_DACLRCK                => CONNECTED_TO_audio_DACLRCK,                --               .DACLRCK
			buttons_export               => CONNECTED_TO_buttons_export,               --        buttons.export
			clk_clk                      => CONNECTED_TO_clk_clk,                      --            clk.clk
			hps_io_hps_io_sdio_inst_CMD  => CONNECTED_TO_hps_io_hps_io_sdio_inst_CMD,  --         hps_io.hps_io_sdio_inst_CMD
			hps_io_hps_io_sdio_inst_D0   => CONNECTED_TO_hps_io_hps_io_sdio_inst_D0,   --               .hps_io_sdio_inst_D0
			hps_io_hps_io_sdio_inst_D1   => CONNECTED_TO_hps_io_hps_io_sdio_inst_D1,   --               .hps_io_sdio_inst_D1
			hps_io_hps_io_sdio_inst_CLK  => CONNECTED_TO_hps_io_hps_io_sdio_inst_CLK,  --               .hps_io_sdio_inst_CLK
			hps_io_hps_io_sdio_inst_D2   => CONNECTED_TO_hps_io_hps_io_sdio_inst_D2,   --               .hps_io_sdio_inst_D2
			hps_io_hps_io_sdio_inst_D3   => CONNECTED_TO_hps_io_hps_io_sdio_inst_D3,   --               .hps_io_sdio_inst_D3
			hps_io_hps_io_uart0_inst_RX  => CONNECTED_TO_hps_io_hps_io_uart0_inst_RX,  --               .hps_io_uart0_inst_RX
			hps_io_hps_io_uart0_inst_TX  => CONNECTED_TO_hps_io_hps_io_uart0_inst_TX,  --               .hps_io_uart0_inst_TX
			hps_io_hps_io_uart0_inst_CTS => CONNECTED_TO_hps_io_hps_io_uart0_inst_CTS, --               .hps_io_uart0_inst_CTS
			hps_io_hps_io_uart0_inst_RTS => CONNECTED_TO_hps_io_hps_io_uart0_inst_RTS, --               .hps_io_uart0_inst_RTS
			memory_mem_a                 => CONNECTED_TO_memory_mem_a,                 --         memory.mem_a
			memory_mem_ba                => CONNECTED_TO_memory_mem_ba,                --               .mem_ba
			memory_mem_ck                => CONNECTED_TO_memory_mem_ck,                --               .mem_ck
			memory_mem_ck_n              => CONNECTED_TO_memory_mem_ck_n,              --               .mem_ck_n
			memory_mem_cke               => CONNECTED_TO_memory_mem_cke,               --               .mem_cke
			memory_mem_cs_n              => CONNECTED_TO_memory_mem_cs_n,              --               .mem_cs_n
			memory_mem_ras_n             => CONNECTED_TO_memory_mem_ras_n,             --               .mem_ras_n
			memory_mem_cas_n             => CONNECTED_TO_memory_mem_cas_n,             --               .mem_cas_n
			memory_mem_we_n              => CONNECTED_TO_memory_mem_we_n,              --               .mem_we_n
			memory_mem_reset_n           => CONNECTED_TO_memory_mem_reset_n,           --               .mem_reset_n
			memory_mem_dq                => CONNECTED_TO_memory_mem_dq,                --               .mem_dq
			memory_mem_dqs               => CONNECTED_TO_memory_mem_dqs,               --               .mem_dqs
			memory_mem_dqs_n             => CONNECTED_TO_memory_mem_dqs_n,             --               .mem_dqs_n
			memory_mem_odt               => CONNECTED_TO_memory_mem_odt,               --               .mem_odt
			memory_mem_dm                => CONNECTED_TO_memory_mem_dm,                --               .mem_dm
			memory_oct_rzqin             => CONNECTED_TO_memory_oct_rzqin,             --               .oct_rzqin
			reset_reset_n                => CONNECTED_TO_reset_reset_n,                --          reset.reset_n
			seven_segments_export        => CONNECTED_TO_seven_segments_export         -- seven_segments.export
		);

