/*
 * Copyright (C) 2014, Michele Balistreri
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include "MRF24J40.h"
#include "osnp.h"
#include "tlv.h"

#include <xc.h>
#include <stdlib.h>

static uint8_t rx_frame_buf[128];

static uint8_t wdt_en;
static uint8_t radio_sleep_en;
static int8_t timer_periods;
static int16_t calibration_offset;

bool transmitting;

#define CALIBRATION_OFFSET_ADDR 14
#define DEVICE_INFO_ADDR 72

__EEPROM_DATA(0x70, 0x3d, 0x62, 0x63, 0x96, 0x2c, 0x9b, 0xf2);
__EEPROM_DATA(0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00);
__EEPROM_DATA(0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07);
__EEPROM_DATA(0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F);
__EEPROM_DATA(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
__EEPROM_DATA(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
__EEPROM_DATA(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
__EEPROM_DATA(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
__EEPROM_DATA(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
__EEPROM_DATA(0x28, 0xA0, 0x26, 0x81, 0x02, 0x00, 0x01, 0x82);
__EEPROM_DATA(0x01, 0xA2, 0xC0, 0x09, 'G', 'r', 'a', 'd');
__EEPROM_DATA('u', 's', 'n', 'i', 'k', 0xC1, 0x12, 'M');
__EEPROM_DATA('i', 'c', 'h', 'e', 'l', 'e', ' ', 'B');
__EEPROM_DATA('a', 'l', 'i', 's', 't', 'r', 'e', 'r');
__EEPROM_DATA('i', 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);

#define load_calibration_offset(buf) load_eeprom(buf, CALIBRATION_OFFSET_ADDR, 2)
#define write_calibration_offset(buf) write_eeprom(buf, CALIBRATION_OFFSET_ADDR, 2)

void load_eeprom(uint8_t *buf, uint16_t addr, uint16_t len) {
  EECON1bits.CFGS = 0;
  EECON1bits.EEPGD = 0;

  for (uint16_t i = 0; i < len; i++) {
    EEADR = addr++;
	EECON1bits.RD = 1;
    buf[i] = EEDATA;
  }
}

void write_eeprom(uint8_t *buf, uint16_t addr, uint16_t len) {
  INTCONbits.GIE = 0;
  EECON1bits.EEPGD = 0;
  EECON1bits.CFGS = 0;

  for (uint16_t i = 0; i < len; i++) {
	EEADR = addr++;
	EEDATA = buf[i];
	EECON1bits.WREN = 1;
    EECON2 = 0x55;
    EECON2 = 0xAA;
	EECON1bits.WR = 1;
	while(EECON1bits.WR);
	EECON1bits.WREN = 0;
  }

  INTCONbits.GIE = 1;
}

uint8_t spi_read() {
  uint8_t tmp;
  tmp = SSPBUF;
  PIR1bits.SSPIF = 0;
  SSPBUF = 0x00;
  while(!PIR1bits.SSPIF);
  return SSPBUF;
}

void spi_write(uint8_t data) {
  uint8_t tmp;
  tmp = SSPBUF;
  PIR1bits.SSPIF = 0;
  SSPBUF = data;
  while( !PIR1bits.SSPIF );
}

void enable_wdt(uint8_t periods, bool radio_can_sleep) {
  wdt_en = true;
  radio_sleep_en = radio_can_sleep;
  timer_periods = periods;
}

void disable_wdt(void) {
  wdt_en = false;
  radio_sleep_en = false;
}

uint16_t process_get_device_info(uint8_t *buf) {
  uint8_t addr = DEVICE_INFO_ADDR;

  EEADR = addr++;
  EECON1 = 0x01;
  uint16_t len = EEDATA;

  load_eeprom(buf, addr, len);

  return len;
}

void write_temperature(uint8_t *buf) {
  ADCON0bits.GO = 1;
  while(ADCON0bits.GO);
  uint16_t adc = (((uint16_t) ADRESH) <<8) | (ADRESL);

  short v = (adc << 5); // roughly 3.2mV per step, so we multiply by 32 to convert to 1/10 mV which fits in 15bits
  short r = v % 100;

  // Maximum accuracy from chip (when calibrated, which is not yet) is 0.5
  // and we read a 10-bit value from ADC so giving anything more
  // accurate would be a lie
  if (r <= 25) {
    v -= r;
  } else {
    v += (100 - r);

    if (r <= 75) {
      v -= 50;
    }
  }

  v /= 10;

  // 500mV is our 0 point
  v -= 500;
  v += calibration_offset;
  
  *buf++ = ((uint16_t) v) >> 8;
  *buf++ = v & 0xff;
};

uint16_t process_get_data(uint8_t *buf) {
  *buf++ = 0xA2;
  *buf++ = 0x0C;
  *buf++ = 0xA1;
  *buf++ = 0x0A;
  *buf++ = 0x80;
  *buf++ = 0x01;
  *buf++ = 0x01;
  *buf++ = 0x81;
  *buf++ = 0x01;
  *buf++ = 0x0A;
  *buf++ = 0x82;
  *buf++ = 0x02;
  write_temperature(buf);
  buf += 2;

  return 14;
}

uint16_t process_configure(uint8_t *in_buf) {
  //TODO: crude, but should work
  if (in_buf[3] == 0x82) {
    calibration_offset = (in_buf[4] << 8) | in_buf[5];
    write_calibration_offset((uint8_t *)&calibration_offset);
  }

  return 0;
}

void write_error(uint8_t tag, uint8_t err, ieee802_15_4_frame_t *resp_frame, uint16_t *resp_off) {
  *resp_off += tlv_write_tag(&resp_frame->payload[*resp_off], tag);
  *resp_off += tlv_write_length(&resp_frame->payload[*resp_off], 3);
  *resp_off += tlv_write_tag(&resp_frame->payload[*resp_off], 0x9E);
  *resp_off += tlv_write_length(&resp_frame->payload[*resp_off], 1);
  resp_frame->payload[*resp_off++] = err;
}

void process_command(ieee802_15_4_frame_t *frame, uint16_t *in_off, ieee802_15_4_frame_t *resp_frame, uint16_t *resp_off, uint8_t associated) {
  uint16_t tag;
  uint16_t len;

  *in_off += tlv_read_tag(&frame->payload[*in_off], &tag);
  *in_off += tlv_read_length(&frame->payload[*in_off], &len);
  *in_off += len;

  switch(tag) {
    case OSNP_GET_DEVICE_INFO:
      *resp_off += process_get_device_info(&resp_frame->payload[*resp_off]);
      break;
    case OSNP_GET_DATA:
      if (!associated) {
        write_error(tag, OSNP_SECURITY_ERROR, resp_frame, resp_off);
      } else {
        *resp_off += process_get_data(&resp_frame->payload[*resp_off]);
      }
      break;
    case OSNP_CONFIGURE:
      if (!associated) {
        write_error(tag, OSNP_SECURITY_ERROR, resp_frame, resp_off);
      } else {
        process_configure(&frame->payload[*in_off]);
      }
      break;
    default:
      write_error(tag, OSNP_UNSUPPORTED_COMMAND, resp_frame, resp_off);
      break;
  }
}

void interrupt isr(void) {
  if (INTCON3bits.INT1F) {
    INTCON3bits.INT1F = 0;
    int interrupts = mrf24j40_int_tasks();
    
    if (interrupts & MRF24J40_INT_RX) {
      if (!mrf24j40_rx_sec_fail()) {
        osnp_frame_received_cb(rx_frame_buf, mrf24j40_rxpkt_intcb(rx_frame_buf, NULL, NULL));
      } else {
        mrf24j40_rxfifo_flush();
      }
    }

    if (interrupts & MRF24J40_INT_TX) {
      transmitting = false;
      osnp_frame_sent_cb(mrf24j40_txpkt_intcb());
    }

    if (interrupts & MRF24J40_INT_SEC) {
      mrf24j40_sec_intcb(true);
    }
  }
}

void main(void) {
  UCONbits.USBEN = 0;
  INTCONbits.GIE = 0;
  OSCCONbits.IRCF = 0b111;
  RCONbits.IPEN = 0;
  INTCON3bits.INT1E = 1;
  INTCON2bits.INTEDG1 = 0;
  
  TRISA = 0;
  TRISB = 0x10;
  TRISC = 0x0A;

  LATA = 0;
  LATB = 0;
  LATC = 0;

  WPUA = 0;
  WPUB = 0;
  
  ADCON0 = 0b00011101;
  ADCON1 = 0;
  ADCON2 = 0b10101010;
  ANSELH = 0;
  ANSEL = 0b10000000;

  SSPSTATbits.CKE = 1;
  SSPCON1 = 0x20;

  load_calibration_offset((uint8_t *)&calibration_offset);
  mrf24j40_initialize();
  mrf24j40_set_cipher(OSNP_SECURITY_LEVEL, OSNP_SECURITY_LEVEL);
  osnp_initialize();
  
  INTCON3bits.INT1F = 0;
  INTCONbits.GIE = 1;

  while(1) {
    while(transmitting) {
      __delay_ms(1);
    }

    if (radio_sleep_en) {
      mrf24j40_sleep(0);
    }

    if (wdt_en) {
      WDTCONbits.SWDTEN = 1;
    }

    SLEEP();    
    WDTCONbits.SWDTEN = 0;
    
    if (!RCONbits.TO) {
      timer_periods--;

      if (timer_periods <= 0) {
        if (radio_sleep_en) {
          mrf24j40_wakeup(0);
        }

        wdt_en = false;
        radio_sleep_en = false;
        osnp_timer_expired_cb();
      }
    }
  }
}

