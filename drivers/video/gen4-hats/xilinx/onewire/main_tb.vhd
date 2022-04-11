--------------------------------------------------------------------------------
-- Company: 
-- Engineer:
--
-- Create Date:   14:24:25 06/23/2013
-- Design Name:   
-- Module Name:   C:/Users/LENOVO/Documents/CPLD/spi-lcd1/main_tb1.vhd
-- Project Name:  spi-lcd1
-- Target Device:  
-- Tool versions:  
-- Description:   
-- 
-- VHDL Test Bench Created by ISE for module: main
-- 
-- Dependencies:
-- 
-- Revision:
-- Revision 0.01 - File Created
-- Additional Comments:
--
-- Notes: 
-- This testbench has been automatically generated using types std_logic and
-- std_logic_vector for the ports of the unit under test.  Xilinx recommends
-- that these types always be used for the top-level I/O of a design in order
-- to guarantee that the testbench will bind correctly to the post-implementation 
-- simulation model.
--------------------------------------------------------------------------------
LIBRARY ieee;
USE ieee.std_logic_1164.ALL;
 
-- Uncomment the following library declaration if using
-- arithmetic functions with Signed or Unsigned values
--USE ieee.numeric_std.ALL;
 
ENTITY main_tb IS
END main_tb;
 
	ARCHITECTURE behavior OF main_tb IS 
 
    -- Component Declaration for the Unit Under Test (UUT)
 
    COMPONENT main
    PORT ( 	SPI_CS : in  STD_LOGIC;
				SPI_SCLK : in  STD_LOGIC;
				SPI_MOSI : in  STD_LOGIC;
				SPI_MISO : inout  STD_LOGIC;
			  
				LCD_CS : out  STD_LOGIC;
				LCD_RESET : out  STD_LOGIC;
				LCD_RS : out  STD_LOGIC;
				ONEWIRE : out  STD_LOGIC;
				LCD_RD : out  STD_LOGIC;
				LCD_WR : out  STD_LOGIC;
				LCD_DB : out  STD_LOGIC_VECTOR (15 downto 0);
			  
				SW : in STD_LOGIC_VECTOR (4 downto 0);
				SW_IRQ : out  STD_LOGIC);
    END COMPONENT;
    

   --Inputs
	signal SW_IRQ : std_logic := '1';
   signal SPI_CS : std_logic := '1';
   signal SPI_SCLK : std_logic := '0';
   signal SPI_MOSI : std_logic := '0';
   --signal SW : std_logic_vector(4 downto 0) := (others => '0');
	signal SW : std_logic_vector(4 downto 0) := "01010";
	
	--BiDirs
   signal SPI_MISO : std_logic;

 	--Outputs
   signal LCD_CS : std_logic;
	signal LCD_WR : std_logic;
   signal LCD_DB : std_logic_vector(15 downto 0);
	signal ONEWIRE : std_logic;

   -- Clock period definitions
   constant CLK_period : time := 500 ns;
	
	shared variable I : integer;
	shared variable Z : std_logic_vector(7 downto 0); 
  
   
BEGIN
 
	-- Instantiate the Unit Under Test (UUT)
   uut: main PORT MAP (
			 SW_IRQ => SW_IRQ,
          SW => SW,
			 SPI_MISO => SPI_MISO,
			 SPI_CS => SPI_CS,
          SPI_SCLK => SPI_SCLK,
          SPI_MOSI => SPI_MOSI,
          LCD_CS => LCD_CS,
			 LCD_WR => LCD_WR,
          LCD_DB => LCD_DB,
			 ONEWIRE => ONEWIRE
        );




   -- Stimulus process
   stim_proc: process
   
	procedure send_spi (Z: std_logic_vector(7 downto 0)) is 
   begin
	   I := 6;
		SPI_MOSI <= Z(7);
		while (I >= -1) loop
			wait for CLK_period/2;
			SPI_SCLK <= '1';       
			wait for CLK_period/2;
			SPI_SCLK <= '0';
			if (I >= 0) then 
				SPI_MOSI <= Z(I);
			else
				wait for CLK_period/2;
			end if;
			I := I - 1;
	 	end loop;
	 end send_spi; 
	 
	 begin		
		-- hold reset state for 100 ns.
		wait for 900 ns;
		SPI_CS <= '0';
		wait for 100 ns;
		
		--- 1-W
		send_spi("11101001");
		send_spi("11111111");
		send_spi("11111111");
		
		-- address 01110010
		send_spi("00000011");
		send_spi("00111111");
		send_spi("00111111");
		send_spi("00111111");
		send_spi("00000011");
		send_spi("00000011");
		send_spi("00111111");
		send_spi("00000011");
			
		-- EOS + TSTART (2x2us min)
		send_spi("00000000");
		send_spi("11111111");
		
		-- 000DDDDD
		send_spi("00000011");
		send_spi("00000011");
		send_spi("00000011");
		send_spi("00111111");
		send_spi("00000011");
		send_spi("00111111");
		send_spi("00000011");
		send_spi("00111111");

		-- EOS
		send_spi("00000001");
		
		wait for 100 ns;
		SPI_CS <= '1';
		wait for 200 ns;
		SPI_CS <= '0';
		wait for 100 ns;
		
      -- data
		send_spi("11100001");
		
		send_spi("11010101");
		send_spi("01011110");
		
		-- repeated
		send_spi("00010010");
		send_spi("01001001");

		send_spi("11111100");
		send_spi("00011111");
		
		send_spi("00000000");
    
		wait for 100 ns;
		SPI_CS <= '1';
		wait for 200 ns;
			
		SPI_CS <= '0';
		wait for 100 ns;

		send_spi("10100001");
		
		send_spi("01010101");
		send_spi("01011111");
		
		-- repeated
		--send_spi("10010010");
		--send_spi("01001001");

		--send_spi("01111100");
		--send_spi("00011111");
		
		send_spi("00000000");
		
		wait for 100 ns;
		SPI_CS <= '1';
		wait for 200 ns;
		
		SPI_CS <= '0';
		wait for 100 ns;

		send_spi("00000001");
		
		send_spi("01100110");
		send_spi("10011001");
		
		-- repeated
		--send_spi("10010010");
		--send_spi("01001001");

		--send_spi("01111100");
		--send_spi("00011111");
		
		wait for 100 ns;
		SPI_CS <= '1';
		wait for 200 ns;
		
		wait;	
   end process;

END;
