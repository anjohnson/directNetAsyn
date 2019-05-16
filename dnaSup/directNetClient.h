/******************************************************************************

Project:
    DirectNet ASYN

File:
    directNetClient.h

Description:
    Header describing client support routines for directNet over ASYN.

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

#ifndef INC_directNetClient_H
#define INC_directNetClient_H

#include <shareLib.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char *dn_error_strings[];

/* These structures hide the ASYN internals from the higher level code */
struct dnAsynClient;
struct plcProto;

struct plcMessage {
    const char *port;
    struct dnAsynClient *pClient;
    const struct plcProto *proto;
    int cmd;
    int addr;
    int len; /* in bytes */
    char *pdata;
    int status;
    void (*callback)(struct plcMessage *pPvt);
    void (*connstat)(struct plcMessage *pPvt, int connected);
};

epicsShareFunc int initDnAsynClient(struct plcMessage* pPlcMsg);
epicsShareFunc int dnAsynClientSend(struct plcMessage *pPlcMsg);

epicsShareExtern const struct plcProto dnpProto, simProto;

#ifdef __cplusplus
}
#endif

#endif /* INC_directNetClient_H */
