/*
    ChibiOS - Copyright (C) 2006..2018 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "ch.h"
#include "hal.h"
#include "rt_test_root.h"
#include "oslib_test_root.h"

#include "ff.h"
#include "file_utils.h"
#include <time.h>
#include <stdio.h>

int align_buffer(void);
void copy_buffer(void);
void request_write(void);
void fwrite_string(char *pString);
void start_log(void);
int read_config_file(void);
int init_sd(void);

unsigned char bLogging = 0; // if =1 than we logging to SD card

//------------------------------------------------------------------------------
// CAN instance configuration
static CANConfig cancfg = {
  CAN_MCR_ABOM | CAN_MCR_AWUM,
  CAN_BTR_SJW(0) | CAN_BTR_TS2(2) |
  CAN_BTR_TS1(1) | CAN_BTR_BRP(13)
};


//------------------------------------------------------------------------------

/*===========================================================================*/
// data buffering functions

#define SD_WRITE_BUFFER             (1024*49)//(1024*21)   // 21K
#define SD_WRITE_BUFFER_FLUSH_LIMIT (1024*48)//(1024*20)   // 20K

#include <string.h>
//#include "mmcsd.h"

// buffer for collecting data to write
char sd_buffer[SD_WRITE_BUFFER];
WORD sd_buffer_length = 0;

// buffer for storing ready to write data
char sd_buffer_for_write[SD_WRITE_BUFFER];
unsigned char bReqWrite = 0; // write request, the sd_buffer is being copied to sd_buffer_for_write
WORD sd_buffer_length_for_write = 0;

unsigned char bWriteFault = 0; // in case of overlap or write fault


// fill buffer with spaces (before \r\n) to make it 512 byte size
// return 1 if filled and ready to write
int align_buffer()
{
  int len;

  if (sd_buffer_length < 2) return 0;
  if (sd_buffer[sd_buffer_length-2] != '\r') return 0;
  if (sd_buffer[sd_buffer_length-1] != '\n') return 0;

  len = MMCSD_BLOCK_SIZE - (sd_buffer_length % MMCSD_BLOCK_SIZE);
  sd_buffer[sd_buffer_length - 2] = ',';
  for (int i = 1; i < len; i++)
    sd_buffer[sd_buffer_length + i - 2] = ' ';
  sd_buffer[sd_buffer_length + len - 2] = '\r';
  sd_buffer[sd_buffer_length + len - 1] = '\n';

  sd_buffer_length += len;

  return 1;
}

// copy input buffer into the buffer for flash writing data
void copy_buffer()
{
  // request write operation
  memcpy(sd_buffer_for_write, sd_buffer, sd_buffer_length);
  sd_buffer_length_for_write = sd_buffer_length;
  sd_buffer_length = 0;
}

void request_write()
{
  if (bReqWrite)
    bWriteFault = 1; // buffer overlapping

  // request write operation
  align_buffer();
  copy_buffer();
  bReqWrite = 1;
}

int iLastWriteSecond = 0;

void fwrite_string(char *pString)
{
  WORD length = strlen(pString);

  // Add string
  memcpy(&sd_buffer[sd_buffer_length], pString, length);
  sd_buffer_length += length;

  // Check flush limit
  if(sd_buffer_length >= SD_WRITE_BUFFER_FLUSH_LIMIT)
  {
    request_write();
  }
}

// file writing
FATFS SDC_FS;
FIL *file;
FRESULT fres;

int iSecond;
#define STRLINE_LENGTH 1024
char sLine[STRLINE_LENGTH];
systime_t stLastWriting;
unsigned char bIncludeTimestamp = 1;

void start_log()
{
  // open file and write the beginning of the load
  RTCDateTime timespec;
  struct tm timp;
  rtcGetTime(&RTCD1, &timespec);
  rtcConvertDateTimeToStructTm(&timespec, &timp, NULL);
  sprintf(sLine, "%04d-%02d-%02dT%02d-%02d-%02dZ.csv", timp.tm_year + 1900, timp.tm_mon, timp.tm_mday, timp.tm_hour, timp.tm_min, timp.tm_sec); // making new file

  file = fopen_(sLine, "a");
  if (bIncludeTimestamp)
    strcpy(sLine, "Timestamp,ID,Data0,Data1,Data2,Data3,Data4,Data5,Data6,Data7\r\n");
  else
    strcpy(sLine, "ID,Data0,Data1,Data2,Data3,Data4,Data5,Data6,Data7\r\n");
  fwrite_string(sLine);
  align_buffer();
  fwrite_(sd_buffer, 1, sd_buffer_length, file);
  f_sync(file);

  // reset buffer counters
  sd_buffer_length_for_write = 0;
  sd_buffer_length = 0;

  bWriteFault = 0;

  stLastWriting = chVTGetSystemTime(); // record time when we did write

  bLogging = 1;
}

int iFilterMask = 0;
int iFilterValue = 0;
unsigned char bLogStdMsgs = 1;
unsigned char bLogExtMsgs = 1;

int read_config_file()
{
  int value;
  char name[128];
  int baud;
  int res = 0;
  int ack = 0;

  iFilterMask = 0;
  iFilterValue = 0;
  bIncludeTimestamp = 1;
  bLogStdMsgs = 1;
  bLogExtMsgs = 1;


  // read file
  file = fopen_("Config.txt", "r");
  if (file == 0) return 0;

  while( f_gets(sLine, STRLINE_LENGTH, file) )
  {
    if (sscanf(sLine, "%s %d", name, &value) == 0)
    {
      continue;
    }

    if (strcmp(name, "baud") == 0)
    {
      baud = value;
      res = 1; // at least we got baudrate, config file accepted
    }
    else
    if (strcmp(name, "ack_en")  == 0)
    {
      ack = value;
    }
    else
    if (strcmp(name, "id_filter_mask")  == 0)
    {
      iFilterMask = value;
    }
    else
    if (strcmp(name, "id_filter_value")  == 0)
    {
      iFilterValue = value;
    }
    else
    if (strcmp(name, "timestamp")  == 0)
    {
      bIncludeTimestamp = value;
    }
    else
    if (strcmp(name, "log_std")  == 0)
    {
      bLogStdMsgs = value;
    }
    else
    if (strcmp(name, "log_ext")  == 0)
    {
      bLogExtMsgs = value;
    }
  }

  // configure CAN
  baud = (int)(7*baud/1000.0 + 0.5); // prescaler value
  if (ack)
    cancfg.btr =  CAN_BTR_SJW(0) | CAN_BTR_TS2(2) | CAN_BTR_TS1(1) | CAN_BTR_BRP(baud - 1);
  else
    cancfg.btr =  CAN_BTR_SJW(0) | CAN_BTR_TS2(2) | CAN_BTR_TS1(1) | CAN_BTR_BRP(baud - 1) | CAN_BTR_SILM; // silent mode flag
  canStop(&CAND2);
  canStart(&CAND2, &cancfg);
  fclose_(file);

  return res;
}

int init_sd()
{
  // initializing SDC interface
  sdcStart(&SDCD1, NULL);
  if (sdcConnect(&SDCD1) == HAL_FAILED)
  {
    return 0;
  }

  // mount the file system
  fres = f_mount(&SDC_FS, "/", 0);
  if (fres != FR_OK)
  {
    sdcDisconnect(&SDCD1);
    return 0;
  }

  return 1;
}

//------------------------------------------------------------------------------

char sTmp[128];

static THD_WORKING_AREA(can1_rx_wa, 256);
static THD_FUNCTION(can1_rx, p) {
  event_listener_t el;
  CANRxFrame rxmsg;

  (void)p;
  chRegSetThreadName("receiver can 1");
  chEvtRegister(&CAND2.rxfull_event, &el, 0);
  while(!chThdShouldTerminateX())
  {
    // if (chEvtWaitAnyTimeout(ALL_EVENTS, MS2ST(100)) == 0) continue;
    chEvtWaitAny(ALL_EVENTS);

    while (canReceive(&CAND2, CAN_ANY_MAILBOX, &rxmsg, TIME_IMMEDIATE) == MSG_OK)
    {
      /* Process message.*/

      if (bLogging)
      {
        // checking message acceptance
        if (rxmsg.IDE)
        {
          // message with extended ID received

          // are we accepting extended ID?
          if (!bLogExtMsgs) continue;

          // then check filter conditions
          if ((rxmsg.EID & iFilterMask) != (iFilterValue & iFilterMask)) continue;
        }
        else
        {
          // message with standard ID received

          // are we accepting standard ID?
          if (!bLogStdMsgs) continue;

          // then check filter conditions
          if ((rxmsg.SID & iFilterMask) != (iFilterValue & iFilterMask)) continue;
        }

        // write down data
        if (bIncludeTimestamp)
          sprintf(sTmp, "%d,%X", (int)chVTGetSystemTime(), rxmsg.EID);
        else
          sprintf(sTmp, "%X", rxmsg.EID);

        for (int i = 0; i < rxmsg.DLC; i++)
        {
          sprintf(sTmp+strlen(sTmp), ",%02X", rxmsg.data8[i]);
        }

        strcat(sTmp, "\r\n");
        fwrite_string(sTmp);
      }
    }
  }
  chEvtUnregister(&CAND2.rxfull_event, &el);
}

//------------------------------------------------------------------------------

/*
 * Application entry point.
 */
int main(void)
{
  halInit();
  chSysInit();

  canStart(&CAND1, &cancfg);
  canStart(&CAND2, &cancfg);
  chThdCreateStatic(can1_rx_wa, sizeof(can1_rx_wa), NORMALPRIO + 7, can1_rx, NULL);

  if (init_sd()) // trying to initialize SD card
  {
    if (read_config_file()) // trying to read configuration file
    {
      // all done -- start logging
      start_log();
    }
  }

  while (1)
  {
    // maybe we need to write log, because we didn't for long time?
    if (chVTTimeElapsedSinceX(stLastWriting) > TIME_S2I(2))
    {
      if (sd_buffer_length > 0) // there is data to write
      {
        // request write operation
        request_write();
      }
    }

    if (bReqWrite)
    {
        if (fwrite_(sd_buffer_for_write, 1, sd_buffer_length_for_write, file) != sd_buffer_length_for_write)
          bWriteFault = 2;
        if (f_sync(file) != FR_OK)
          bWriteFault = 2;
        bReqWrite = 0;

        stLastWriting = chVTGetSystemTime(); // record time when we did write
    }
  }
}
//------------------------------------------------------------------------------
