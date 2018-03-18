//=====================================================================//
/*!	@file
	@brief	デジタル・マイク・レシーバー
    @author 平松邦仁 (hira@rvf-rc45.net)
	@copyright	Copyright (C) 2017, 2018 Kunihito Hiramatsu @n
				Released under the MIT license @n
				https://github.com/hirakuni45/RL78/blob/master/LICENSE
*/
//=====================================================================//
#include "common/renesas.hpp"
#include "common/port_utils.hpp"
#include "common/itimer.hpp"
#include "common/fifo.hpp"
#include "common/uart_io.hpp"
#include "common/tau_io.hpp"
#include "common/iica_io.hpp"
#include "common/csi_io.hpp"
#include "common/format.hpp"
#include "common/command.hpp"
#include "common/switch_man.hpp"
#include "common/spi_io.hpp"
#include "common/flash_io.hpp"
#include "common/flash_man.hpp"

#include "chip/PCM1772.hpp"
#include "chip/SEGMENT.hpp"

#define SOFT_SPI

namespace {

	static const uint16_t VERSION = 16;

	static const uint8_t SD1X_DELAY = 6;  // 100ms
	static const uint8_t SD2X_DELAY = 6;  // 100ms

	static const uint8_t SEND_CH_DELAY = 6;  // 100ms

	typedef device::itimer<uint8_t> ITM;
	ITM		itm_;

	// UART の定義
	typedef utils::fifo<uint8_t, 64> BUFFER;
	typedef device::uart_io<device::SAU00, device::SAU01, BUFFER, BUFFER> UART0;
	typedef device::uart_io<device::SAU02, device::SAU03, BUFFER, BUFFER> UART1;
	typedef device::uart_io<device::SAU10, device::SAU11, BUFFER, BUFFER> UART2;
	UART0	uart0_;
	UART1	uart1_;
	UART2	uart2_;

	// フラッシュ入出力
	typedef device::flash_io FLASH;
	FLASH	flash_;

	struct frec_t {
		uint8_t		ch1_;
		uint8_t		ch2_;

		frec_t() : ch1_(0), ch2_(0) { }
	};
	typedef utils::flash_man<frec_t>  FLASH_MAN;
	FLASH_MAN	flash_man_(flash_);
	frec_t		frec_;

	// 出力ポート定義
	typedef device::PORT<device::port_no::P7, device::bitpos::B3> SG1_A;
	typedef device::PORT<device::port_no::P7, device::bitpos::B2> SG1_B;
	typedef device::PORT<device::port_no::P7, device::bitpos::B1> SG1_C;
	typedef device::PORT<device::port_no::P7, device::bitpos::B0> SG1_D;
	typedef device::PORT<device::port_no::P5, device::bitpos::B2> SG1_E;
	typedef device::PORT<device::port_no::P5, device::bitpos::B1> SG1_F;
	typedef device::PORT<device::port_no::P5, device::bitpos::B0> SG1_G;
	typedef chip::SEGMENT<SG1_A, SG1_B, SG1_C, SG1_D, SG1_E, SG1_F, SG1_G, device::NULL_PORT> SEG1;

	typedef device::PORT<device::port_no::P12,device::bitpos::B6> SG2_A;
	typedef device::PORT<device::port_no::P12,device::bitpos::B5> SG2_B;
	typedef device::PORT<device::port_no::P3, device::bitpos::B1> SG2_C;
	typedef device::PORT<device::port_no::P7, device::bitpos::B7> SG2_D;
	typedef device::PORT<device::port_no::P7, device::bitpos::B6> SG2_E;
	typedef device::PORT<device::port_no::P7, device::bitpos::B5> SG2_F;
	typedef device::PORT<device::port_no::P7, device::bitpos::B4> SG2_G;
	typedef chip::SEGMENT<SG2_A, SG2_B, SG2_C, SG2_D, SG2_E, SG2_F, SG2_G, device::NULL_PORT> SEG2;

	// 入力ポート定義
	typedef device::PORT<device::port_no::P1, device::bitpos::B0> LED_M1;
	typedef device::PORT<device::port_no::P2, device::bitpos::B7> MUT1;
	typedef device::PORT<device::port_no::P2, device::bitpos::B4> LED_M2;
	typedef device::PORT<device::port_no::P2, device::bitpos::B3> MUT2;

	typedef device::PORT<device::port_no::P4, device::bitpos::B6> CH_SEL_U;
	typedef device::PORT<device::port_no::P4, device::bitpos::B5> IR_TEST;
	typedef device::PORT<device::port_no::P4, device::bitpos::B4> CH_SCAN;
	typedef device::PORT<device::port_no::P4, device::bitpos::B3> L_M;

	typedef device::PORT<device::port_no::P12, device::bitpos::B4> SD11;
	typedef device::PORT<device::port_no::P12, device::bitpos::B3> SD12;
	typedef device::PORT<device::port_no::P13, device::bitpos::B7> SD21;
	typedef device::PORT<device::port_no::P12, device::bitpos::B2> SD22;

	typedef device::PORT<device::port_no::P12, device::bitpos::B1> MSEL;

	enum class INPUT_TYPE : uint8_t {
		CH_SEL_U,
		IR_TEST,
		CH_SCAN,

		SD11,
		SD12,
		SD21,
		SD22,

		L_M,	///< MIC/LINE

		MSEL,	///< モジュール選択、L:1個、H:2個

		PNC_IN0,	///< ポップノイズキャンセル、入力０(P03)
		PNC_IN1,	///< ポップノイズキャンセル、入力１(P152)
	};

	typedef utils::switch_man<uint16_t, INPUT_TYPE> INPUT;
	INPUT	input_;

	// ポップ・ノイズ解消用ポート
	typedef device::PORT<device::port_no::P2,  device::bitpos::B1> PNC_CTL0;
	typedef device::PORT<device::port_no::P13, device::bitpos::B0> PNC_CTL1;
	typedef device::PORT<device::port_no::P0,  device::bitpos::B3> PNC_IN0;
	typedef device::PORT<device::port_no::P15, device::bitpos::B2> PNC_IN1;

	uint8_t	pnc_ctl0_count_;
	uint8_t	pnc_ctl1_count_;

	uint8_t	ch_no_[2];

	// 仕様
	// P03/Low（L）で　⇒　P21/Lに遷移
	// P03/Hi （H）で　⇒　50ms後にP21/Hに遷移

	// P152/L      で　⇒　P130/Lに遷移
	// P152/H      で　⇒　50ms後にP130/Hに遷移
	void service_pop_noise_cancel_()
	{
		if(!input_.get_level(INPUT_TYPE::PNC_IN0)) {
			PNC_CTL0::P = 0;
		} else if(input_.get_positive(INPUT_TYPE::PNC_IN0)) {
			pnc_ctl0_count_ = 3;
		}
		if(pnc_ctl0_count_ > 0) {
			--pnc_ctl0_count_;
		} else {
			PNC_CTL0::P = 1;
		}

		if(!input_.get_level(INPUT_TYPE::PNC_IN1)) {
			PNC_CTL1::P = 0;
		} else if(input_.get_positive(INPUT_TYPE::PNC_IN1)) {
			pnc_ctl1_count_ = 3;
		}
		if(pnc_ctl1_count_ > 0) {
			--pnc_ctl1_count_;
		} else {
			PNC_CTL1::P = 1;
		}
	}


	void service_input_()
	{
		uint16_t lvl = 0;

		if(CH_SEL_U::P() ) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::CH_SEL_U);
		if(IR_TEST::P()) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::IR_TEST);
		if(CH_SCAN::P()) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::CH_SCAN);

		if(L_M::P()) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::L_M);

		if(SD11::P() ) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::SD11);
		if(SD12::P() ) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::SD12);
		if(SD21::P() ) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::SD21);
		if(SD22::P() ) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::SD22);

		if(MSEL::P()) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::MSEL);

		if(PNC_IN0::P()) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::PNC_IN0);
		if(PNC_IN1::P()) lvl |= 1 << static_cast<uint16_t>(INPUT_TYPE::PNC_IN1);

		input_.service(lvl);
	}

	// PCM1772 デバイス選択（P34 ---> PCM1772:/MS(15)）
	typedef device::PORT<device::port_no::P3, device::bitpos::B4> PCM_SEL;
	// PCM1772 パワー・ダウン（P32 ---> PCM1772:/PD)
	typedef device::PORT<device::port_no::P3, device::bitpos::B2> PCM_PD;
#ifdef SOFT_SPI
	typedef device::NULL_PORT MISO;
	typedef device::PORT<device::port_no::P3, device::bitpos::B5> MOSI;
	typedef device::PORT<device::port_no::P3, device::bitpos::B3> SPCK;
	typedef device::spi_io<MISO, MOSI, SPCK, device::soft_spi_mode::CK01> SPI;
	SPI		spi_;

	typedef chip::PCM1772<SPI, PCM_SEL, PCM_PD> PCM;
	PCM		pcm_(spi_);
#else
	// CSI(SPI) の定義、CSI30 の通信では、「SAU12」を利用、１ユニット、チャネル２
	// ※出力のみ
	typedef device::csi_io<device::SAU12, device::manage::csi_port::OUTPUT> CSI;
	CSI		csi_;

	typedef chip::PCM1772<CSI, PCM_SEL, PCM_PD> PCM;
	PCM		pcm_(csi_);
#endif

	bool	sd11_;
	bool	sd12_;
	bool	sd21_;
	bool	sd22_;
	uint8_t	sd1x_count_;
	uint8_t	sd2x_count_;

	utils::command<64> command_;

	uint8_t	fw_delay_;


	void write_flash_()
	{
		frec_.ch1_ = ch_no_[0];
		frec_.ch2_ = ch_no_[1];
		flash_man_.write(frec_);
	}


	void read_flash_()
	{
		flash_man_.read(frec_);
		ch_no_[0] = frec_.ch1_;
		ch_no_[1] = frec_.ch2_;
	}


	void create_ch_msg_(uint8_t ch, char* dst)
	{
		++ch;
		dst[0] = 0x43;
		dst[1] = (ch / 10) + '0';
		dst[2] = (ch % 10) + '0';
		dst[3] = 0x0d;
		dst[4] = 0x0a;
		dst[5] = 0x00;
	}


	void send_ch_()
	{
		char tmp[8];
		create_ch_msg_(ch_no_[0], tmp);
		uart1_.puts(tmp);
		utils::format("Send M0 (UART1): %s") % tmp;
		create_ch_msg_(ch_no_[1], tmp);
		uart2_.puts(tmp);
		utils::format("Send M1 (UART2): %s") % tmp;
	}
}


extern "C" {

	void sci_putch(char ch)
	{
		uart0_.putch(ch);
	}


	void sci_puts(const char* str)
	{
		uart0_.puts(str);
	}


	char sci_getch(void)
	{
		return uart0_.getch();
	}


	uint16_t sci_length()
	{
		return uart0_.recv_length();
	}


	INTERRUPT_FUNC void UART0_TX_intr(void)
	{
		uart0_.send_task();
	}


	INTERRUPT_FUNC void UART0_RX_intr(void)
	{
		uart0_.recv_task();
	}


	INTERRUPT_FUNC void UART0_ER_intr(void)
	{
		uart0_.error_task();
	}


	INTERRUPT_FUNC void UART1_TX_intr(void)
	{
		uart1_.send_task();
	}


	INTERRUPT_FUNC void UART1_RX_intr(void)
	{
		uart1_.recv_task();
	}


	INTERRUPT_FUNC void UART1_ER_intr(void)
	{
		uart1_.error_task();
	}


	INTERRUPT_FUNC void UART2_TX_intr(void)
	{
		uart2_.send_task();
	}


	INTERRUPT_FUNC void UART2_RX_intr(void)
	{
		uart2_.recv_task();
	}


	INTERRUPT_FUNC void UART2_ER_intr(void)
	{
		uart2_.error_task();
	}


	INTERRUPT_FUNC void ITM_intr(void)
	{
		itm_.task();
	}
};


int main(int argc, char* argv[])
{
//	utils::port::pullup_all();  ///< 安全の為、全ての入力をプルアップ

	// itimer の開始
	{
		uint8_t intr_level = 1;
		itm_.start(60, intr_level);
	}

	// UART0 の開始
	{
		uint8_t intr_level = 1;
		uart0_.start(115200, intr_level);
	}

	// UART1 の開始
	{
		uint8_t intr_level = 1;
		uart1_.start(19200, intr_level);
	}
	// UART2 の開始
	{
		uint8_t intr_level = 1;
		uart2_.start(19200, intr_level);
	}

	device::PFSEG0 = 0x00;
	device::PFSEG1 = 0x00;
	device::PFSEG2 = 0x00;
	device::PFSEG3 = 0x00;
	device::PFSEG4 = 0x00;
	device::PFSEG5 = 0x00;
	device::PFSEG6 = 0x00;

	SEG1::start();
	SEG2::start();

	LED_M1::DIR = 1;
	MUT1::DIR = 1;
	LED_M2::DIR = 1;
	MUT2::DIR = 1;

	device::ADPC = 0b001; // A/D input all digital port
	PNC_CTL0::DIR = 1; 
	PNC_CTL0::PMC = 0;  // digital in/out
//	PNC_CTL1::DIR = 1;  // only output

	utils::format("\nStart Digital MIC Reciver Version: %d.%02d\n")
		% (VERSION / 100) % (VERSION % 100);

#ifdef SOFT_SPI
	spi_.start(1000000);
#else
	// CSI 開始
	{
		uint8_t intr_level = 0;
//		if(!csi_.start(4000000, CSI::PHASE::TYPE4, intr_level)) {
		if(!csi_.start(1000000, CSI::PHASE::TYPE1, intr_level)) {
			utils::format("CSI Start fail...\n");
		}
	}
#endif

	// PCM1772 初期化
	pcm_.start();

	// フラッシュ開始
	if(flash_.start()) {
		utils::format("Flash I/O start: size: %d bytes, bank: %d\n")
			% FLASH::data_flash_size % FLASH::data_flash_bank;
		flash_man_.start();
	} else {
		utils::format("Flash I/O start error\n");
	}

	command_.set_prompt("# ");

	// input 関係初期化
	sd11_ = 1;
	sd12_ = 1;
	sd21_ = 1;
	sd22_ = 1;
	sd1x_count_ = 0;
	sd2x_count_ = 0;

	// チャネル関係
	ch_no_[0] = 0;
	ch_no_[1] = 0;
	fw_delay_ = 0;
	read_flash_();
	for(uint8_t i = 0; i < SEND_CH_DELAY; ++i) {
		itm_.sync();
	}
	send_ch_();

	while(1) {
		itm_.sync();

		service_input_();

		if(input_.get_positive(INPUT_TYPE::CH_SEL_U)) {
			utils::format("CH_SEL_U\n");
		}
		if(input_.get_positive(INPUT_TYPE::IR_TEST)) {
			utils::format("IR_TEST\n");
		}
		if(input_.get_positive(INPUT_TYPE::CH_SCAN)) {
			utils::format("CH_SCAN\n");
		}

		if(input_.get_turn(INPUT_TYPE::L_M)) {
			utils::format("L_M: %d\n") % input_.get_level(INPUT_TYPE::L_M);
		}
		if(input_.get_turn(INPUT_TYPE::SD11)) {
			utils::format("SD11: %d\n") % input_.get_level(INPUT_TYPE::SD11);
		}
		if(input_.get_turn(INPUT_TYPE::SD12)) {
			utils::format("SD12: %d\n") % input_.get_level(INPUT_TYPE::SD12);
		}
		if(input_.get_turn(INPUT_TYPE::SD21)) {
			utils::format("SD21: %d\n") % input_.get_level(INPUT_TYPE::SD21);
		}
		if(input_.get_turn(INPUT_TYPE::SD22)) {
			utils::format("SD22: %d\n") % input_.get_level(INPUT_TYPE::SD22);
		}
		if(input_.get_turn(INPUT_TYPE::MSEL)) {
			utils::format("MSEL: %d\n") % input_.get_level(INPUT_TYPE::MSEL);
		}

		service_pop_noise_cancel_();

		// LED_M1, MUT1 制御（100ms遅延）
		{
			bool sd11 = input_.get_level(INPUT_TYPE::SD11);
			bool sd12 = input_.get_level(INPUT_TYPE::SD12);
			if(sd11 == 1 && sd12 == 1) {
				LED_M1::P = 1;
				MUT1::P   = 0;
			} else if(sd11_ != sd11 || sd12_ != sd12) {
				sd1x_count_ = SD1X_DELAY;
				utils::format("SD1x trigger\n");
			}
			if(sd1x_count_ > 0) {
				sd1x_count_--;
				if(sd1x_count_ == 0) {
					if(sd11 == 0 && sd12 == 0) {
						LED_M1::P = 0;
						MUT1::P   = 1;
					} else if(sd11 == 0 && sd12 == 1) {
						LED_M1::P = 0;
						MUT1::P   = 1;
					} else if(sd11 == 1 && sd12 == 0) {
						LED_M1::P = 0;
						MUT1::P   = 1;
					}
				}
			}
			sd11_ = sd11;
			sd12_ = sd12;

			bool sd21 = input_.get_level(INPUT_TYPE::SD21);
			bool sd22 = input_.get_level(INPUT_TYPE::SD22);
			if(sd21 == 1 && sd22 == 1) {
				LED_M2::P = 1;
				MUT2::P   = 0;
			} else if(sd21_ != sd21 || sd22_ != sd22) {
				sd2x_count_ = SD2X_DELAY;
				utils::format("SD2x trigger\n");
			}
			if(sd2x_count_ > 0) {
				sd2x_count_--;
				if(sd2x_count_ == 0) {
					if(sd21 == 0 && sd22 == 0) {
						LED_M2::P = 0;
						MUT2::P   = 1;
					} else if(sd21 == 0 && sd22 == 1) {
						LED_M2::P = 0;
						MUT2::P   = 1;
					} else if(sd21 == 1 && sd22 == 0) {
						LED_M2::P = 0;
						MUT2::P   = 1;
					}
				}
			}
			sd21_ = sd21;
			sd22_ = sd22;
		}

		uint8_t ch_pad[2];
		ch_pad[0] = ch_no_[0];
		ch_pad[1] = ch_no_[1];
		{  // チャネル関係制御
			uint8_t msel = MSEL::P() & 1;
			uint8_t no = ch_no_[msel];
			if(input_.get_positive(INPUT_TYPE::CH_SEL_U)) {
				++no;
				if(no >= 32) no = 0;
			}
			ch_no_[msel] = no;
			++no;
			SEG1::decimal(no / 10);
			SEG2::decimal(no % 10);
		}
		if(ch_pad[0] != ch_no_[0] || ch_pad[1] != ch_no_[1]) {
			fw_delay_ = 180;
		}
		if(fw_delay_ > 0) {
			fw_delay_--;
			if(fw_delay_ == 0) {
				write_flash_();
			}
		}

		// コマンド入力と、コマンド解析
		if(command_.service()) {
			uint8_t cmdn = command_.get_words();
			if(cmdn >= 1) {
				if(command_.cmp_word(0, "help")) {
					utils::format("---\n");
				}
			}
		}

#if 0
		while(uart1_.recv_length() > 0) {
			uint8_t d = uart1_.getch();
			utils::format("UART1: 0x%02X\n") % static_cast<uint16_t>(d);
		}
		while(uart2_.recv_length() > 0) {
			uint8_t d = uart2_.getch();
			utils::format("UART2: 0x%02X\n") % static_cast<uint16_t>(d);
		}
#endif
	}
}
