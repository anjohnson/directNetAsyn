/******************************************************************************

Project:
    DirectNet ASYN

File:
    devDnAsyn.h

Description:
    Header desribing common device support routines for directNet over ASYN

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

#ifndef INC_devDnAsyn_H
#define INC_devDnAsyn_H

/* Required include files */
#include <dbCommon.h>
#include <devSup.h>
#include <link.h>
#include <shareLib.h>


#define PLCWORDBITS	16	/* Bits per Word */
#define PLCWORDMASK	0xffff	/* All word bits set */
#define PLCWORDBYTES	2	/* Bytes per Word */
#define DNREFOFFSET	1	/* PLC Address offset over DirectNet */


/* limits */
#define MAXDNSLAVEID 	90	/* DirectNet limitation */

#define WRITEMINADDR	02000	/* Don't write below V02000 */
#define WRITEMAXADDR	02777	/* Don't write above V02777 */


typedef struct {
    dset common;
    long (*read_write)(struct dbCommon *prec);
    long (*special_linconv)(struct dbCommon *prec, int after);
} XXDSET;


extern int devDnAsynDebug;


/* Device information structures */

struct plcProto;

struct plcInfo {
    struct plcInfo *pNext;
    const char* name;
    const char* port;
    unsigned short slaveId;
    unsigned short alarm;
    const struct plcProto *proto;
    struct rdCache *rdCache;
    struct wrCache *wrCache;
    unsigned long nRdReqs;
    unsigned long nWrReqs;
    unsigned long nSuccess;
    unsigned long nDnFail;
    unsigned long nAsynFail;
    int connflag;
};

struct plcAddr {
    struct plcInfo *plcInfo;
    unsigned short vAddr;
    unsigned char bitNum;
};

typedef void (*dnPlcReportFn)(int detail, struct plcInfo *pPlc);


/* Common routines in devDnAsyn.c */

epicsShareFunc int createDnAsynPLC(
    const char* pname, int slaveId, const char* port);
epicsShareFunc int createDnAsynSimulatedPLC(
    const char* pname, int slaveId, const char* port);

epicsShareFunc struct plcInfo * dnAsynPlc(const char* pname);
epicsShareFunc int dnAsynAddr(
    struct dbCommon *prec, struct plcAddr *paddr, struct link *plink);
epicsShareFunc void dnAsynReport(
    int detail, dnPlcReportFn ioReport);

#endif /* INC_devDnAsyn_H */
