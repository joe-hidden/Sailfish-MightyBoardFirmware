/*
 * Copyright 2010 by Adam Mayer	 <adam@makerbot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "Main.hh"
#include "Host.hh"
#include "Command.hh"
#include <avr/interrupt.h>
#include <util/atomic.h>
#include <avr/wdt.h>
#include "Timeout.hh"
#include "Steppers.hh"
#include "Motherboard.hh"
#include "SDCard.hh"
#include "Eeprom.hh"
#include "EepromMap.hh"
#include "TemperatureTable.hh"
#include <util/delay.h>
#include "UtilityScripts.hh"
#include "Piezo.hh"

#ifdef HAS_I2C_LCD
#include "TWI.hh"
#endif

#if defined(STACK_PAINT) && defined(DEBUG_SRAM_MONITOR)
	bool stackAlertLockout = false;
	uint16_t stackAlertCounter = 0;
#endif

#ifdef STACK_PAINT

        //Stack checking
        //http://www.avrfreaks.net/index.php?name=PNphpBB2&file=viewtopic&t=52249
        extern uint8_t _end;
        extern uint8_t __stack;

        #define STACK_CANARY 0xc5

        void StackPaint(void) __attribute__ ((naked)) __attribute__ ((section (".init1")));

        void StackPaint(void)
        {
                #if 0
                        uint8_t *p = &_end;

                        while(p <= &__stack)
                        {
                                *p = STACK_CANARY;
                                p++;
                        }
                #else
                        __asm volatile ("    ldi r30,lo8(_end)\n"
                                        "    ldi r31,hi8(_end)\n"
                                        "    ldi r24,lo8(0xc5)\n" /* STACK_CANARY = 0xc5 */
                                        "    ldi r25,hi8(__stack)\n"
                                        "    rjmp .cmp\n"
                                        ".loop:\n"
                                        "    st Z+,r24\n"
                                        ".cmp:\n"
                                        "    cpi r30,lo8(__stack)\n"
                                        "    cpc r31,r25\n"
                                        "    brlo .loop\n"
                                        "    breq .loop"::);
                #endif
        }


        uint16_t StackCount(void)
        {
                const uint8_t *p = &_end;
                uint16_t       c = 0;

                while(*p == STACK_CANARY && p <= &__stack)
                {
                        p++;
                        c++;
                }

                return c;
        }

#endif

#ifdef HAS_I2C_LCD
// Historically, TWI was initialized in the RGB_LED code.
// However, we're intending to use TWI for more than just one device.
// So initialize it early.
//
// This needs to be done before Motherboard class is instantiated,
// because the LCD is initiatlized in a C++ initializer list.
// Since motherboard is intiialized as a global variable, usie .init1 section
// to initialize it before main();
void initialize_twi(void) __attribute__ ((naked)) __attribute__ ((section (".init1")));
void initialize_twi(void) {
	// Initialize TWI
	TWI_init();
	
	// Enable pull-ups on the TWI interface (if being used on a board without
	// discrete pull-up resistors on the TWI bus.)
	// PORTD |= 0b11;
}
#endif

void reset(bool hard_reset) {
	ATOMIC_BLOCK(ATOMIC_FORCEON) {
		
	//	bool brown_out = false;
	//	uint8_t resetFlags = MCUSR & 0x0f;
	//	// check for brown out reset flag and report if true
	//	if(resetFlags & (1 << 2)){
	//		brown_out = true;
	//	}
		
        // clear watch dog timer and re-enable
		if(hard_reset)
		{ 
            // ATODO: remove disable
			wdt_disable();
			MCUSR = 0x0;
			wdt_enable(WDTO_8S); // 8 seconds is max timeout
		}
		
		// initialize major classes
		Motherboard& board = Motherboard::getBoard();	
		sdcard::reset();
		Piezo::reset();
		utility::reset();
		command::reset();
#ifndef ERASE_EEPROM_ON_EVERY_BOOT
		eeprom::init();
#endif
		steppers::init();
		steppers::abort();
		steppers::reset();
//		initThermistorTables();
		board.reset(hard_reset);
		
	// brown out occurs on normal power shutdown, so this is not a good message		
	//	if(brown_out)
	//	{
	//		board.getInterfaceBoard().errorMessage("Brown-Out Reset     Occured", 27);
	//		board.startButtonWait();
	//	}	
	}
}

int main() {
#ifdef ERASE_EEPROM_ON_EVERY_BOOT
        eeprom::erase();
	return 0;
#endif

	Motherboard& board = Motherboard::getBoard();
#ifdef REVG
	INTERFACE_POWER.setDirection(true);
	INTERFACE_POWER.setValue(false);
#endif
	board.init();
	reset(true);
	sei();
	while (1) {
		// Host interaction thread.
		host::runHostSlice();
		// Command handling thread.
		command::runCommandSlice();
		// Motherboard slice
		board.runMotherboardSlice();
                // Stepper slice
                steppers::runSteppersSlice();

		//Alert if SRAM/stack has been corrupted by running out of SRAM
#if defined(STACK_PAINT) && defined(DEBUG_SRAM_MONITOR)
		stackAlertCounter ++;
		if ( stackAlertCounter >= 5000 ) {
			if (( ! stackAlertLockout ) && ( StackCount() == 0 )) {
				stackAlertLockout = true;
				Piezo::errorTone(6);
			}
			stackAlertCounter = 0;
		}
#endif

		// Piezo slice
		Piezo::runPiezoSlice();

		// reset the watch dog timer
		wdt_reset();
	}
	return 0;
}
