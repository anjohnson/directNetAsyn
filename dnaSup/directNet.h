/******************************************************************************

Project:
    DirectNet ASYN

File:
    directNet.h

Description:
    Definitions for directNet messages.

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

#ifndef INC_directNet_h
#define INC_directNet_h

/*
 * Control Characters used in DirectNet protocol
 */

#define SOHCHAR 0x01
#define STXCHAR 0x02
#define ETXCHAR 0x03
#define EOTCHAR 0x04
#define ENQCHAR 0x05
#define ACKCHAR 0x06
#define NAKCHAR 0x15
#define ETBCHAR 0x17
#define SEQCHAR 'N'

#define HEADER_LEN  17
#define BLOCK_LEN   256
#define SLAVEOFFSET 0x20

/*
 * Standard timeouts in milliseconds
 */

#define ENQACKTIME   800
#define HDRACKTIME  2000
#define DATACKTIME 20000
#define ACKEOTTIME   800

/*
 * Other parameters
 */

#define MAX_RETRIES 3

#endif /* INC_directNet_h */
