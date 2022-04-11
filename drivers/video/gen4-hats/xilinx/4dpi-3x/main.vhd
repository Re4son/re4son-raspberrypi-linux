----------------------------------------------------------------------------------
-- Company: 
-- Engineer: 
-- 
-- Create Date:    08:47:39 06/19/2013 
-- Design Name: 
-- Module Name:    main - Behavioral 
-- Project Name: 
-- Target Devices: 
-- Tool versions: 
-- Description: 
--
-- Dependencies: 
--
-- Revision:  
-- Revision 0.01 - File Created
-- Additional Comments: 
--
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.STD_LOGIC_UNSIGNED.ALL;

-- Uncomment the following library declaration if using
-- arithmetic functions with Signed or Unsigned values
--use IEEE.NUMERIC_STD.ALL;

-- Uncomment the following library declaration if instantiating
-- any Xilinx primitives in this code.
--library UNISIM;
--use UNISIM.VComponents.all;


		 
entity main is
    Port ( 	SPI_CS : in  STD_LOGIC;
				SPI_SCLK : in  STD_LOGIC;
				SPI_MOSI : in  STD_LOGIC;
				SPI_MISO : inout  STD_LOGIC;
			  
				LCD_CS : out  STD_LOGIC;
				LCD_RESET : out  STD_LOGIC;
				LCD_RS : out  STD_LOGIC;
				BACKLIGHT : out  STD_LOGIC;
				LCD_RD : out  STD_LOGIC;
				LCD_WR : out  STD_LOGIC;
				LCD_DB : out  STD_LOGIC_VECTOR (15 downto 0);
			  
				SW : in STD_LOGIC_VECTOR (4 downto 0);
				SW_IRQ : out  STD_LOGIC);
			  
end main;

architecture Behavioral of main is

	-- PRODUCT_CODE is used by Linux driver to identify which module is connected
	-- 0xAB for 4DPi-35 
	-- 0xAC for 4DPi-32
	constant PRODUCT_CODE : STD_LOGIC_VECTOR(7 downto 0) := X"AB";

	signal clock_div : STD_LOGIC_VECTOR(4 downto 0) := B"11000";
   
	signal mosi_shift_reg : STD_LOGIC_VECTOR(15 downto 0) := X"0000";
	signal miso_shift_reg : STD_LOGIC_VECTOR(7 downto 0) := X"00";
		
	signal block_transfer: STD_LOGIC;
	signal compressed_block: STD_LOGIC;
	signal sig_lcd_reset: STD_LOGIC;
	signal sig_lcd_rs: STD_LOGIC;
	signal sig_backlight: STD_LOGIC;
	signal sig_lcd_rd: STD_LOGIC;
	signal sig_lcd_wr: STD_LOGIC;
	signal sig_lcd_db : STD_LOGIC_VECTOR(15 downto 0) := X"0000";
	
	signal control_loaded : boolean := false;
	signal data_loaded : boolean := false;	
	signal pixel: STD_LOGIC;
	
begin
	
   -- CLK
	process (SPI_SCLK, SPI_CS, clock_div(3))
	begin
		if (SPI_CS = '0') then
			
			-- increase SPI clock counter and roll data from MISO shift register
			if (SPI_SCLK'event and SPI_SCLK = '0') then
					clock_div <= clock_div + '1';
					miso_shift_reg(7 downto 1) <= miso_shift_reg(6 downto 0);
					miso_shift_reg(0) <= '0';
			end if;

			if (SPI_SCLK'event and SPI_SCLK = '1') then
			   -- roll data into MOSI shift register
				mosi_shift_reg(15 downto 1) <= mosi_shift_reg(14 downto 0);
				mosi_shift_reg(0) <= SPI_MOSI;
				

				-- when transfering a compressed block first bit of 16-bit transfer determines 
				-- whether a new pixel is beeing transfered (pixel = 1)
				-- or previous pixel is iterated (pixel = 0)
				if (clock_div(3 downto 0) = B"0000" and compressed_block = '1') then
					pixel <= SPI_MOSI;
				end if;
				
				-- generate sig_lcd_wr signal which is used when pixels are not iterated
				if (clock_div(3 downto 0) = B"0001") then
					sig_lcd_wr <= '0';
				end if;
				if (clock_div(3 downto 0) = B"0111") then
					sig_lcd_wr <= '1';	
				end if;
			end if;
			
			-- set control signals
			-- this is done only once after 16 bits have been transfered since beginning of the transfer
			-- control signals are in bits 8-15 of shift register
			if (clock_div(3)'event and clock_div(3) = '1') then
				if(control_loaded = false) then
					control_loaded <= true;
					block_transfer <= mosi_shift_reg(15);
					compressed_block <= mosi_shift_reg(14);
					sig_lcd_reset <= mosi_shift_reg(13);
					sig_lcd_rs <= mosi_shift_reg(12);
					sig_backlight <= mosi_shift_reg(11);
					sig_lcd_rd <= mosi_shift_reg(10);
					-- wr/ is auto generated...
					-- sig_lcd_wr <= mosi_shift_reg(9);		
				end if;
			end if;
			
			
			-- set data signals
			-- every 16 bits values are copied from mosi_shifz_reg to LCD data bus
			if (clock_div(3)'event and clock_div(3) = '0') then		
				-- transfer without compression (array of 16-bit pixels)
				if ((block_transfer = '1' or data_loaded = false) and compressed_block = '0' )then
					sig_lcd_db(15 downto 0) <= mosi_shift_reg(15 downto 0);
				end if;
				
				-- transfer with compression, 15-bit pixel is being transfered
				if ((block_transfer = '1' or data_loaded = false) and compressed_block = '1' and pixel = '1') then
					sig_lcd_db(15 downto 6) <= mosi_shift_reg(14 downto 5);
					--sig_lcd_db(5) <= mosi_shift_reg(9) and mosi_shift_reg(8) and mosi_shift_reg(7) and mosi_shift_reg(6) and mosi_shift_reg(5);
					--sig_lcd_db(5) <= '0';
					sig_lcd_db(5) <= mosi_shift_reg(9);
					sig_lcd_db(4 downto 0) <= mosi_shift_reg(4 downto 0);
				end if;
				data_loaded <= control_loaded;
			end if;
		
		else
			-- get ready for a new transfer
			clock_div <= B"11000";
			control_loaded <= false;
			data_loaded <= false;
			pixel <= '1';
			compressed_block <= '0';
			
			-- if lcd_reset is not active tactile switches can be read from controller
			-- if lcd_reset is active the product code can be read from controller
			if (sig_lcd_reset = '1') then 
				-- LRES is high => switches state
				miso_shift_reg(7 downto 3) <= SW(4 downto 0);
				miso_shift_reg(2 downto 0) <= B"000";
			else
				-- LRES is low => product_code
				miso_shift_reg <= PRODUCT_CODE;
			end if;
		end if;

	end process;
	
	LCD_CS <= SPI_CS;
	LCD_RESET <= sig_lcd_reset;
	LCD_RS <= sig_lcd_rs;
	BACKLIGHT <= sig_backlight;
	LCD_RD <= sig_lcd_rd;
	
	-- Use latched MOSI
	LCD_WR <= sig_lcd_wr when control_loaded = true and block_transfer = '1' and pixel = '1' and clock_div(3 downto 0) > B"0000" else
		mosi_shift_reg(0) when control_loaded = true and block_transfer = '1' and pixel = '0' and clock_div(3 downto 0) > B"0000" else
		'0' when data_loaded = true and block_transfer = '0' else
		'1';
		
	LCD_DB(15 downto 0) <= sig_lcd_db(15 downto 0);
	
	SPI_MISO <= miso_shift_reg(7) when SPI_CS='0' else 'Z';
	
	SW_IRQ <= SW(0) and SW(1) and SW(2) and SW(3) and SW(4);
	
end Behavioral;

