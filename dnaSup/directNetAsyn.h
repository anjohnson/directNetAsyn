/******************************************************************************

Project:
    DirectNet ASYN

File:
    directNetAsyn.h

Description:
    Header describing ASYN interface with directNet.

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

#ifndef INC_directNetAsyn_h
#define INC_directNetAsyn_h

#include "directNet.h"


/* command values */

#define WRITECMD	0x80

#define READVMEM	0x01
#define READINPS	0x02
#define READOUTS	0x03
#define READSPAD	0x06
#define READPROG	0x07
#define READSTAT	0x09

#define WRITEVMEM	(READVMEM | WRITECMD)
#define WRITESPAD	(READSPAD | WRITECMD)
#define WRITEPROG	(READPROG | WRITECMD)

/* status values */

/* DN_SUCCESS must be zero.
 * Values must match dn_error_strings in directnetClient.c */
#define DN_SUCCESS	0
#define DN_INTERNAL	1
#define DN_TIMEOUT	2
#define DN_SEND_FAIL	3
#define DN_SEL_FAIL	4
#define DN_HDR_FAIL	5
#define DN_RDBLK_FAIL	6
#define DN_WRBLK_FAIL	7
#define DN_NOT_EOT	8
#define DN_GOT_EOT	9

/* timeouts in seconds */

#define ENQACKDELAY (ENQACKTIME/1000.0 + 1.0)
#define HDRACKDELAY (HDRACKTIME/1000.0 + 1.0)
#define DATACKDELAY (DATACKTIME/1000.0 + 1.0)
#define ACKEOTDELAY (ACKEOTTIME/1000.0 + 1.0)


/* Misc values */

#define MASTERID	0

#define BAUDRATE	9600
#define BYTERATE	(BAUDRATE/10)

#define DN_RDDATA_MAX	32
#define DN_PLCWORDLEN	2


#endif /* INC_directNetAsyn_h */
