/****************************************************************************
 * Name: JIS-II Card Spoofer
 * Author: 7m4mon (nomulabo.com)
 * Description: This program is a integration of Magspoof and MagCardReader,
 *              and modefied for JIS-II Cards. (Japanese domestic cards)
 *              Additional Functions:
 *              10 cards infomation save/load to internal EEPROM.
 *              Parity and LRC check for MagCardReader.
 *              Display card infomation to OLED(128x32).
 * License: MIT
 * 
 * Special Thanks:
 *     MagSpoof - by Samy Kamkar
 *     https://github.com/samyk/magspoof
 *     MagCardReader - by B. Gian James
 *     http://www.instructables.com/id/Turn-your-Arduino-into-a-Magnetic-Card-Reader/
 *     OLED_FONT: "u8g_font_profont11r"  Copyright (c) 2014 Carl Osterwald, Stephen C. Gilardi, Andrew Welch
*****************************************************************************/

#include <avr/interrupt.h>
#include <common.h>         // from MagCardReader (B. Gian James)
#include <uart.h>           // from MagCardReader (B. Gian James)
#include <string.h>
#include <stdlib.h>
#include <EEPROM.h>
#include <U8glib.h>
#include <avr/sleep.h>

#define CARD_STROBE   PIND2 
#define CARD_PRESENT  PIND3
#define CARD_DATA1    PIND4

#define MAG_L_PIN       14      // as A0, left pin, to Driver IC
#define MAG_R_PIN       15      // as A1, right pin, to Driver IC
#define MAG_EN_PIN      13      // also on-board LED

#define SDA_PIN         18      // as A4
#define SCL_PIN         19      // as A5
#define OLED_RES_PIN    17      // as A3
U8GLIB_SSD1306_128X32 u8g(U8G_I2C_OPT_NONE);  // I2C / TWI 

#define HEX_POS_SW1    11
#define HEX_POS_SW2    9
#define HEX_POS_SW4    12
#define HEX_POS_SW8    10
#define REC_MODE_PIN   8

#define CLOCK_US         400
#define TRACK_SIZE       70     // NTT Track 69字+null
#define PREAMBLE_LENGTH  64
#define POSTAMBLE_LENGTH 64
#define STX              0x7F   // JIS-IIのSTXとETX は0x7F(DEL)
#define ETX              0x7F   // JIS-IIのSTXとETX は0x7F(DEL)

#define MAX_BUFF_SZ1     256    // Recording Buffer. power of 2
#define JIS2_BIT_LEN     8      // JIS-II (7bit + CRC)
#define CHR_BIT_LEN      7      // JIS2_BIT_LEN - CRC

#define EVEN     0
#define ODD      1

/* Local Functions for Reader */
static void MagCardReader(void);
static void InitInterrupt(void);
static void ReadData(uint8_t chk_crc);
static uint8_t ProcessData(void);
static void InitData(void);
static void DebugPrint(const char *);
static unsigned char reverse_bit8(unsigned char x);
static unsigned char count_bit8(unsigned char b8);
static void calc_lrc(char character);

/* Variable for Reader */
char                  buff[MAX_BUFF_SZ1];
volatile int8_t       bit;
volatile uint8_t      idx;
volatile uint8_t      bDataPresent;

/* Local Functions for Spoofer */
void setPolarity(int8_t polarity);
void playBit(int8_t bit);
void play_character(char character);
void playTrack(void);

/* Local Functions for OLED and EEPROM */
static void oled_drawstr(char *text);
static void load_card_info(int index);
static void save_card_info(int index);

/* Variable for Spoofer */
uint8_t polarity = 0;
int lrc = 0xff;
char lrc_send;
uint8_t card_sel;

char* track_str = {
  "123456789012345678901234567890123456789012345678901234567890123456789",  // NTT Track: MAX 69文字
};


int main(){  
  /* Setup Pins for Spoofer */
  pinMode(MAG_EN_PIN, OUTPUT);
  pinMode(MAG_L_PIN, OUTPUT);
  pinMode(MAG_R_PIN, OUTPUT);
  digitalWrite(MAG_EN_PIN, LOW);
  digitalWrite(MAG_L_PIN, LOW);
  digitalWrite(MAG_R_PIN, LOW);

  /* Setup Pins for UI */
  pinMode(OLED_RES_PIN, OUTPUT);
  digitalWrite(OLED_RES_PIN, HIGH);
  pinMode(REC_MODE_PIN, INPUT_PULLUP);
  pinMode(HEX_POS_SW1, INPUT_PULLUP);
  pinMode(HEX_POS_SW2, INPUT_PULLUP);
  pinMode(HEX_POS_SW4, INPUT_PULLUP);
  pinMode(HEX_POS_SW8, INPUT_PULLUP);

  /* Setup Pins for Reader */
  pinMode(CARD_STROBE, INPUT_PULLUP);
  pinMode(CARD_DATA1, INPUT_PULLUP);
  pinMode(CARD_PRESENT, INPUT_PULLUP);
  
  /* カード番号の選択(0～9) */
  card_sel =  digitalRead(HEX_POS_SW1) + digitalRead(HEX_POS_SW2) * 2 + digitalRead(HEX_POS_SW4) * 4 + digitalRead(HEX_POS_SW8) * 8;
  card_sel = 15 - card_sel;                           // 正論理のスイッチほうが安かったから。
  uint8_t rw_sel;
  rw_sel = digitalRead(REC_MODE_PIN);

  USART_init(BAUD_57600, INT_NONE);
  
  if(rw_sel){
    MagCardSpoofer();  
    /* 一回だけ再生し、そのままスリープ */
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();                         // Sets the Sleep Enable bit in the MCUCR Register (SE BIT)
    sleep_cpu();                            // Sleep until reset
  }else{
    MagCardReader();
  }
}

void MagCardSpoofer(void){
  load_card_info(card_sel);
  oled_drawstr(&track_str[0]);
  DebugPrint("Play Start: ");
  DebugPrint(track_str);
  DebugPrint("\r\n");
  playTrack();

  DebugPrint("LRC: ");
  lrc_send &= 0x7F;
  USART_tx(lrc_send);
  DebugPrint("\r\n");
}

void MagCardReader(void)
{
  char message[21];
  sprintf(message,"SWIPE YOUR CARD -> %d",card_sel);
  oled_drawstr(message);

  InitData();
  InitInterrupt();
  
  char cbuff[6];  // general translation buffer
  DebugPrint("System initialized!\r\n");
  DebugPrint("Checking for data after ");
  DebugPrint(itoa((CHECK_TIME/1000),cbuff,10));
  DebugPrint("ms\r\n");

  for (;;)
  {
    if( TCNT1 >= CHECK_TIME)
    { 
      StopTimer();
      ClearTimer();
      
      char parity;
      parity = ProcessData();
      ReadData(parity);     
      
      idx = 0;
      bit = CHR_BIT_LEN;
      bDataPresent = 0;
      memset(&buff,0,MAX_BUFF_SZ1);

    } 
  }
}

uint8_t ProcessData(void)
{
  uint8_t chk_crc = 0;
  for (uint8_t i = 0; i < (idx-1); i++)
  {
    chk_crc += (count_bit8(buff[i]) & 1); // Add Parity bits ( No Error should be zero )
    BCLR(buff[i],0);                      // Clear Parity bit
    buff[i] = reverse_bit8(buff[i]);      // JIS-II cards recorded LSB First
  }

  char cbuff[3];
  DebugPrint("Magnetic card digits: ");
  DebugPrint(itoa(idx,cbuff,10));
  DebugPrint("\r\n");
  DebugPrint("CRC: ");
  DebugPrint(itoa(chk_crc,cbuff,10));
  DebugPrint("\r\n");

  return chk_crc;
}

void ReadData(uint8_t chk_crc)
{
  if(chk_crc != 0){
    DebugPrint("Invalid card format: CRC Error. Try swiping again.\r\n");
    oled_drawstr("ERROR: CRC Error");
    return;
  }

  /* 一度 idxまで行ったものを逆順で確認していく */
  int16_t i = int16_t(idx - 1);
  while (buff[i] != STX && i != 0) --i;
  if (i == 0) {
    DebugPrint("Invalid card format: StartSentinel. Try swiping again.\r\n");
    oled_drawstr("ERROR: StartSentinel");
    return;
  }
  
  lrc = 0xff;                    // reset LRC (global valiable)
  char lrc_read = buff[i+1];
  DebugPrint("LRC Read: ");
  USART_tx(lrc_read);
  DebugPrint("\r\n");
  
  --i;
  calc_lrc(STX);
  while(buff[i] != ETX && i != 0) {
    USART_tx(buff[i]);
    calc_lrc(buff[i]);
    --i;                                      // 逆から再生
  }
  DebugPrint("\r\n");
  if (i < 0) {
    DebugPrint("Invalid card format: StopSentinel. Try swiping again.\r\n");
    oled_drawstr("ERROR: StopSentinel");
    return;
  }
  
  if (idx < TRACK_SIZE - 1) {
    DebugPrint("Invalid card format: TrackSize. Try swiping again.\r\n");
    oled_drawstr("ERROR: TrackSize");
    return;
  }
  calc_lrc(ETX);
  DebugPrint("LRC Calc: ");
  lrc &= 0x7F;
  USART_tx(lrc);
  DebugPrint("\r\n");

  if(lrc != lrc_read){
    DebugPrint("Invalid card format: LRC Missmatch. Try swiping again.\r\n");
    oled_drawstr("ERROR: LRC Missmatch");
    return;
  }
  
  memcpy(track_str, &buff[i+1], TRACK_SIZE);
  track_str[TRACK_SIZE-1] = 0;
  oled_drawstr(&track_str[0]);       // 読み込んだデータを表示
  save_card_info(card_sel);          // EEPROMに保存
    
}

void InitInterrupt(void)
{
  // Setup interrupt
  BSET(EIMSK,INT0); // external interrupt mask
  BSET(EICRA,ISC01);  // falling edge
  BCLR(EICRA,ISC00);  // falling edge

  BSET(SREG,7);   // I-bit in SREG
}

#include <avr/interrupt.h>
ISR(INT0_vect)
{
  StopTimer();
  ClearTimer();

  if ( !BCHK(PIND,CARD_DATA1) ) // inverse low = 1
  {
    BSET(GPIOR0,bit);
    --bit;
    bDataPresent = 1;
  } else if (bDataPresent) {
    BCLR(GPIOR0,bit);
    --bit;
  }

  if (bit < 0) {
    buff[idx] = (char)GPIOR0;
    ++idx;
    bit = CHR_BIT_LEN;
  }
    
  StartTimer();
}

static void InitData(void)
{
  memset(&buff,0,MAX_BUFF_SZ1);
  bit = CHR_BIT_LEN; idx = 0;
  bDataPresent = 0;
  GPIOR0 = 0x00;
}


void DebugPrint(const char * msg)
{
  while (*msg != 0x00)
    USART_tx(*(msg++));
}

unsigned char reverse_bit8(unsigned char x)
{
  x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
  x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
  return (x << 4) | (x >> 4);
}

unsigned char count_bit8(unsigned char b8)
{
  b8 = ((b8 & 0xAA) >> 1) + (b8 & 0x55);
  b8 = ((b8 & 0xCC) >> 2) + (b8 & 0x33);
  b8 = ((b8 & 0xF0) >> 4) + (b8 & 0x0F);
  return b8;
}


#define OLED_ROW_LEN   4     // 行数
#define OLED_COL_LEN  20     // 1行あたりの文字数
void oled_drawstr(char *text){
  char oled_buffer[OLED_ROW_LEN][OLED_COL_LEN + 1];
  u8g.firstPage();
  do {
    boolean text_end = false;
    u8g.setFont(u8g_font_profont11r);
    u8g.setFontPosTop();
    for (uint8_t i =0; i < OLED_ROW_LEN; i++){
      if(!text_end){
        strncpy(&oled_buffer[i][0], text + i * OLED_COL_LEN, OLED_COL_LEN);   // s2の長さが少ない場合は残りをnull埋め
        oled_buffer[i][OLED_COL_LEN] = '\0';                                  // 行の終わりにnullを追加。
       if( (oled_buffer[i][OLED_COL_LEN-1]) == 0){                            // 最後の文字がnullだったらtext終了とみなし、以降null埋め
         text_end = true;
       }
      //テキストが終わっていた場合
      }else{
        for(uint8_t j=0; j<= OLED_COL_LEN; j++){
          oled_buffer[i][j] = 0;              //もし、最終文字がnullだったら以降null埋め
        }
      }
      u8g.setPrintPos( 0, 8 * i -1);         //１行表示。－オフセット1
      u8g.print(oled_buffer[i]);
    }
  } while (u8g.nextPage());
}

#define CARD_INFO_SIZE TRACK_SIZE
void load_card_info(int index){
  int eeprom_addr = index * CARD_INFO_SIZE;
  char track_data[TRACK_SIZE];
  for(int i = 0; i < TRACK_SIZE; i++){
    track_data[i] = EEPROM.read(eeprom_addr++);
  }
  strncpy(&track_str[0],&track_data[0], TRACK_SIZE - 1);    // 69個のデータをコピー
  *(&track_str[0]+(TRACK_SIZE -1)) = 0;                     // 70番目はnullにする。
}

void save_card_info(int index){
  int eeprom_addr = index * CARD_INFO_SIZE;
  char track_data[TRACK_SIZE];
  strncpy(&track_data[0], &track_str[0], TRACK_SIZE - 1);   // 69個のデータをコピー
  track_data[TRACK_SIZE -1] = 0;                            // 70番目はnullにする。
  for(int i = 0; i < TRACK_SIZE; i++){
    EEPROM.write(eeprom_addr++, track_data[i]);
  }
}



// Set the polarity of the elctromagnet.
void setPolarity(int8_t polarity) {
  if (polarity) {
    digitalWrite(MAG_R_PIN, LOW);
    digitalWrite(MAG_L_PIN, HIGH);
  } else {
    digitalWrite(MAG_L_PIN, LOW);
    digitalWrite(MAG_R_PIN, HIGH);
  }
}

// send a single bit out
void playBit(int8_t bit) {
  polarity ^= 1;
  setPolarity(polarity);
  delayMicroseconds(CLOCK_US);

  if (bit == 1) {
    polarity ^= 1;
    setPolarity(polarity);
  }
  delayMicroseconds(CLOCK_US);
}

// plays out a full track, calculating CRCs and LRC
void playTrack(void)
{
  polarity = 0;
  digitalWrite(MAG_EN_PIN, HIGH);
  // First put out a bunch of leading zeros.
  for(uint8_t i = 0; i < PREAMBLE_LENGTH; i++){
    playBit(0);
  }

  play_character(STX);

  for(uint8_t i = 0; track_str[i] != '\0'; i++) {
    play_character(track_str[i]);     // JIS-II cards needs no SHIFT (so, removed "sublen")
  }

  play_character(ETX);

  // finish calculating and send last "byte" (LRC)
  lrc_send = lrc;                     // 上書きされてしまうので一旦保存
  play_character(lrc);
  
  // finish with 0's
  for (uint8_t i = 0; i < POSTAMBLE_LENGTH; i++){
    playBit(0);
  }

  /* 後始末 */
  digitalWrite(MAG_L_PIN, LOW);
  digitalWrite(MAG_R_PIN, LOW);
  digitalWrite(MAG_EN_PIN, LOW);
}


void play_character(char character){
    uint8_t crc = EVEN;
    uint8_t bitlen = JIS2_BIT_LEN;
    for(uint8_t j = 0; j < bitlen-1; j++) {
      crc ^= (character & 1);
      lrc ^= (character & 1) << j;
      playBit(character & 1);
      character >>= 1;
    }
    playBit(crc);
}

void calc_lrc(char character){
    uint8_t bitlen = JIS2_BIT_LEN;
    for(uint8_t j = 0; j < bitlen-1; j++) {
      lrc ^= (character & 1) << j;
      character >>= 1;
   }
}

