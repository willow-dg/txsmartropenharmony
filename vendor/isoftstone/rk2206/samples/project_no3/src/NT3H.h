/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef NT3H_H_
#define NT3H_H_

#include "stdbool.h"
#include <stdint.h>
#include "nfc.h"

#define NT3H1X_SLAVE_ADDRESS    0x55

#define MANUFACTORING_DATA_REG  0x0
#define USER_START_REG          0x1

#define USER_END_REG            0x77
#define CONFIG_REG              0x7A


#define SRAM_START_REG          0xF8
#define SRAM_END_REG            0xFB // just the first 8 bytes

#define SESSION_REG             0xFE
#define SESSION_NS_REG          0x06

/*
 * NS_REG 位序：NT3H1101/1201 与 NT3H2111/2211 plus 相反。
 * 本板 U4 是 NT3H1201W0FHK（原理图 PAGE02）。上一轮按 plus 只看 bit7，
 * 1201 的 RF_FIELD 在 bit0，于是一直 rf=0、还在写卡、巡检不加。
 *
 * 1201：bit7=NDEF_DATA_READ(读 NS_REG 自清) bit6=I2C_LOCKED bit5=RF_LOCKED
 *       bit2=EEPROM_WR_ERR bit1=EEPROM_WR_BUSY bit0=RF_FIELD_PRESENT
 * plus：bit7=RF_FIELD bit6=WR_BUSY bit2=RF_LOCKED bit0=NDEF_DATA_READ
 *
 * 巡检用并集，不含 I2C_LOCKED / WR_BUSY（本机写卡后会置位，会误判有场）。
 */
#define NS_PLUS_NDEF_DATA_READ    (1u << 0)
#define NS_PLUS_I2C_LOCKED        (1u << 1)
#define NS_PLUS_RF_LOCKED         (1u << 2)
#define NS_PLUS_SRAM_I2C_READY    (1u << 3)
#define NS_PLUS_SRAM_RF_READY     (1u << 4)
#define NS_PLUS_EEPROM_WR_ERR     (1u << 5)
#define NS_PLUS_EEPROM_WR_BUSY    (1u << 6)
#define NS_PLUS_RF_FIELD          (1u << 7)

#define NS_1201_RF_FIELD          (1u << 0)
#define NS_1201_EEPROM_WR_BUSY    (1u << 1)
#define NS_1201_EEPROM_WR_ERR     (1u << 2)
#define NS_1201_SRAM_RF_READY     (1u << 3)
#define NS_1201_SRAM_I2C_READY    (1u << 4)
#define NS_1201_RF_LOCKED         (1u << 5)
#define NS_1201_I2C_LOCKED        (1u << 6)
#define NS_1201_NDEF_DATA_READ    (1u << 7)

#define NS_REG_FIELD_HINT   (NS_PLUS_RF_FIELD | NS_PLUS_RF_LOCKED | NS_PLUS_NDEF_DATA_READ | \
                             NS_1201_RF_FIELD | NS_1201_RF_LOCKED | NS_1201_NDEF_DATA_READ)
#define NS_REG_NDEF_HINT    (NS_PLUS_NDEF_DATA_READ | NS_1201_NDEF_DATA_READ)

/* 兼容旧名：plus 位定义 */
#define NS_REG_NDEF_DATA_READ     NS_PLUS_NDEF_DATA_READ
#define NS_REG_I2C_LOCKED         NS_PLUS_I2C_LOCKED
#define NS_REG_RF_LOCKED          NS_PLUS_RF_LOCKED
#define NS_REG_SRAM_I2C_READY     NS_PLUS_SRAM_I2C_READY
#define NS_REG_SRAM_RF_READY      NS_PLUS_SRAM_RF_READY
#define NS_REG_EEPROM_WR_ERR      NS_PLUS_EEPROM_WR_ERR
#define NS_REG_EEPROM_WR_BUSY     NS_PLUS_EEPROM_WR_BUSY
#define NS_REG_RF_FIELD_PRESENT   NS_PLUS_RF_FIELD

#define NFC_PAGE_SIZE           16

typedef enum
{
    NT3HERROR_NO_ERROR,
    NT3HERROR_READ_HEADER,
    NT3HERROR_WRITE_HEADER,
    NT3HERROR_INVALID_USER_MEMORY_PAGE,
    NT3HERROR_READ_USER_MEMORY_PAGE,
    NT3HERROR_WRITE_USER_MEMORY_PAGE,
    NT3HERROR_ERASE_USER_MEMORY_PAGE,
    NT3HERROR_READ_NDEF_TEXT,
    NT3HERROR_WRITE_NDEF_TEXT,
    NT3HERROR_TYPE_NOT_SUPPORTED
} NT3HerrNo;

extern uint8_t      nfcPageBuffer[NFC_PAGE_SIZE];
extern NT3HerrNo    errNo;

/*
 * This strucure is used in the ADD record functionality
 * to store the last nfc page information, in order to continue from that point.
 */
typedef struct
{
    uint8_t page;
    uint8_t usedBytes;
} UncompletePageStr;


typedef struct
{
    RecordPosEnu ndefPosition;
    uint8_t rtdType;
    uint8_t *rtdPayload;
    uint8_t rtdPayloadlength;
    void    *specificRtdData;
} NDEFDataStr;


unsigned int NT3HI2cInit();
unsigned int NT3HI2cDeInit();


void NT3HGetNxpSerialNumber(char* buffer);

/*
 * read the user data from the requested page
 * first page is 0
 *
 * the NT3H1201 has 119 PAges
 * the NT3H1101 has 56 PAges (but the 56th page has only 8 Bytes)
*/
bool NT3HReadUserData(uint8_t page);
bool NT3HReadUserDataQuiet(uint8_t page);

/*
 * Write data information from the starting requested page.
 * If the dataLen is bigger of NFC_PAGE_SIZE, the consecuiteve needed
 * pages will be automatically used.
 *
 * The functions stops to the latest available page.
 *
 first page is 0
 * the NT3H1201 has 119 PAges
 * the NT3H1101 has 56 PAges (but the 56th page has only 8 Bytes)
*/
bool NT3HWriteUserData(uint8_t page, const uint8_t* data);

/*
 * The function read the first page of user data where is stored the NFC Header.
 * It is important because it contains the total size of all the stored records.
 *
 * param endRecordsPtr return the value of the total size excluding the NDEF_END_BYTE
 * param ndefHeader    Store the NDEF Header of the first record
 */
bool NT3HReadHeaderNfc(uint8_t *endRecordsPtr, uint8_t *ndefHeader);

/*
 * The function write the first page of user data where is stored the NFC Header.
 * update the bytes that contains the payload Length and the first NDEF Header
 *
 * param endRecordsPtr The value of the total size excluding the NDEF_END_BYTE
 * param ndefHeader    The NDEF Header of the first record
 */
bool NT3HWriteHeaderNfc(uint8_t endRecordsPtr, uint8_t ndefHeader);

bool getSessionReg(void);
bool getNxpUserData(char* buffer);
bool NT3HReadSram(void);
bool NT3HReadSession(void);
bool NT3HReadConfiguration(uint8_t *configuration);

bool NT3HEraseAllTag(void);

bool NT3HReaddManufactoringData(uint8_t *manuf) ;

bool NT3HResetUserData(void);

#endif /* NFC_H_ */
