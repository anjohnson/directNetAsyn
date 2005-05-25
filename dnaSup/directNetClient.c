/******************************************************************************

Project:
    DirectNet ASYN

File:
    directNetClient.c

Description:
    Client support routines for directNet over ASYN

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <errlog.h>
#include <asynDriver.h>
#include <asynOctet.h>

#include "directNetAsyn.h"

#define epicsExportSharedSymbols
#include "directNetClient.h"


typedef struct dnAsynClient {
    asynUser *pau;
    asynOctet *poctet;
    void *drvPvt;
} dnAsynClient;


const char *dn_error_strings[] = {
    "DN_SUCCESS",
    "DN_INTERNAL",
    "DN_TIMEOUT",
    "DN_SEND_FAIL",
    "DN_SEL_FAIL",
    "DN_HDR_FAIL",
    "DN_RDBLK_FAIL",
    "DN_WRBLK_FAIL",
    "DN_NOT_EOT"
};


/* asynOctet interface routines */

static int dnpSend(dnAsynClient *pclient, const char *pdata, int len) {
    asynUser *pau = pclient->pau;
    asynStatus status;
    size_t wrote;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpSend(%p, %p, %d)\n", pclient, pdata, len);
    
    pau->timeout += len / BYTERATE;
    status = pclient->poctet->writeRaw(pclient->drvPvt, pau, pdata, len, &wrote);
    if (status == asynSuccess) {
	asynPrintIO(pau, ASYN_TRACEIO_DEVICE, pdata, len,
		    "dnpSend: sent %u of %d bytes\n", wrote, len);
	return DN_SUCCESS;
    } else {
	asynPrint(pau, ASYN_TRACE_ERROR,
		    "dnpSend: write failed: %s\n", pau->errorMessage);
	return DN_SEND_FAIL;
    }
}

static int dnpGets(dnAsynClient *pclient, char *pdata, int len) {
    asynUser *pau = pclient->pau;
    asynStatus status;
    size_t got;
    int why;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpGets(%p, %p, %d)\n", pclient, pdata, len);
    
    status = pclient->poctet->readRaw(pclient->drvPvt, pau, pdata, len, &got, &why);
    if (status == asynSuccess) {
	int trace = ASYN_TRACEIO_DEVICE;
	int retval = 0;
	if (got != len) {
	    trace |= ASYN_TRACE_ERROR;
	    retval = -1;
	}
	asynPrintIO(pau, trace, pdata, got,
		    "dnpGets: Got %u of %d bytes, reason 0x%x\n",
		    got, len, why);
	return retval;
    } else if (status == asynTimeout) {
	struct plcMessage* pMsg = (struct plcMessage*) pau->userPvt;
	asynPrint(pau, ASYN_TRACE_FLOW,
		  "dnpGets: Read timeout from Asyn port \"%s\"\n", 
		  pMsg->port);
    } else {
	asynPrint(pau, ASYN_TRACE_ERROR,
		    "dnpGets: Read failed: %s\n", pau->errorMessage);
    }
    return -1;
}

static int dnpGetc(dnAsynClient *pclient) {
    char reply[1];
    int result;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpGetc(%p)\n", pclient);
    
    result = dnpGets(pclient, reply, 1);
    return result ? result : reply[0];
}

static int dnpSendGetc(dnAsynClient *pclient, char *pdata, int len) {
    asynUser *pau = pclient->pau;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpSendGetc(%p, %p, %d)\n", pclient, pdata, len);
    
    pclient->poctet->flush(pclient->drvPvt, pau);
    dnpSend(pclient, pdata, len);
    return dnpGetc(pclient);
}


/* Protocol implementation */

static char dnpLRC(dnAsynClient *pclient, const char *pdata, int len) {
    char lrc = 0;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpLRC(%p, %p, %d)\n", pclient, pdata, len);
    
    while (len--) {
	lrc ^= *pdata++;
    }
    return lrc;
}

static int dnpSelect(dnAsynClient *pclient, int target) {
    asynUser *pau = pclient->pau;
    char reselect[4], *select = &reselect[1];
    int retries = MAX_RETRIES;
    int slaveId = target + SLAVEOFFSET;
    int sendlen = 3;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpSelect(%p, %d)\n", pclient, target);
    
    reselect[0] = EOTCHAR;
    reselect[1] = SEQCHAR;
    reselect[2] = slaveId;
    reselect[3] = ENQCHAR;
    pau->timeout = ENQACKDELAY;
    do {
	int reply = dnpSendGetc(pclient, select, sendlen);
	while (reply > 0 && reply != SEQCHAR && reply != EOTCHAR) {
	    asynPrint(pau, ASYN_TRACE_ERROR,
		      "dnpSelect: Not SEQ/EOT - %d\n", reply);
	    reply = dnpGetc(pclient);
	}
	if (reply == SEQCHAR &&
	    slaveId == dnpGetc(pclient) &&
	    ACKCHAR == dnpGetc(pclient)) {
	    return DN_SUCCESS;
	}
	select = reselect;
	sendlen = 4;
    } while (--retries > 0);
    return DN_SEL_FAIL;
}

static int dnpHeader(dnAsynClient *pclient, int cmd, int addr, int len) {
    asynUser *pau = pclient->pau;
    int reply, retries = MAX_RETRIES;
    char header[HEADER_LEN];
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpHeader(%p, %d, %d, %d)\n", pclient, cmd, addr, len);
    
    reply = dnpSelect(pclient, (cmd >> 8) & 0xff);
    if (reply) return reply;
    
    sprintf((char *) header, "%c%4.4X%4.4X%4.4X%2.2X%c", 
	    SOHCHAR, cmd, addr, len, MASTERID, ETBCHAR);
    header[HEADER_LEN-1] = dnpLRC(pclient, &header[1], HEADER_LEN - 3);
    pau->timeout = HDRACKDELAY;
    do {
	reply = dnpSendGetc(pclient, header, HEADER_LEN);
	if (reply == ACKCHAR) return DN_SUCCESS;
    } while (reply == NAKCHAR && --retries > 0);
    return DN_HDR_FAIL;
}

static int dnpWrBlk(dnAsynClient *pclient, const char *pdata, int len) {
    asynUser *pau = pclient->pau;
    char block[BLOCK_LEN + 3];
    int blen = len;
    int reply, retries = MAX_RETRIES;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpWrBlk(%p, %p, %d)\n", pclient, pdata, len);
    
    if (blen > BLOCK_LEN) blen = BLOCK_LEN;
    len -= blen;
    
    block[0] = STXCHAR;
    memcpy(&block[1], pdata, blen);
    block[blen+1] = len ? ETBCHAR : ETXCHAR;
    block[blen+2] = dnpLRC(pclient, &block[1], blen);
    pau->timeout = DATACKDELAY;
    do {
	reply = dnpSendGetc(pclient, block, blen + 3);
	if (reply == ACKCHAR) return DN_SUCCESS;
    } while (reply == NAKCHAR && --retries > 0);
    return DN_WRBLK_FAIL;
}

static int dnpRdBlk(dnAsynClient *pclient, char *pdata, int len) {
    asynUser *pau = pclient->pau;
    int blen = len;
    int reply, retries = MAX_RETRIES;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpRdBlk(%p, %p, %d)\n", pclient, pdata, len);
    
    if (blen > BLOCK_LEN) blen = BLOCK_LEN;
    len -= blen;
    pau->timeout = DATACKDELAY;
    do {
	static const char ack = ACKCHAR, nak = NAKCHAR;
	reply = dnpGetc(pclient);
	while (reply != STXCHAR && reply > 0) {
	    asynPrint(pau, ASYN_TRACE_ERROR, "dnpRdBlk: Not STX - %d\n", reply);
	    reply = dnpGetc(pclient);
	}
	if ((reply >= 0) &&
	    (dnpGets(pclient, pdata, blen) == DN_SUCCESS) &&
	    (dnpGetc(pclient) == (len ? ETBCHAR : ETXCHAR)) &&
	    (dnpGetc(pclient) == dnpLRC(pclient, pdata, blen))) {
	    return dnpSend(pclient, &ack, 1);
	}
	
	reply = dnpSend(pclient, &nak, 1);
    } while (--retries > 0);
    return DN_RDBLK_FAIL;
}

static void dnpEOT(dnAsynClient *pclient) {
    char data[] = {EOTCHAR};
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpEOT(%p)\n", pclient);
    
    dnpSend(pclient, data, 1);
}


/* Protocol interface routines */

static int dnpWrite(dnAsynClient *pclient, int cmd, int addr, const char *pdata, int len) {
    int status;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpWrite(%p)\n", pclient);
    
    status = dnpHeader(pclient, cmd, addr, len);
    if (status == DN_SUCCESS) {
	do {
	    status = dnpWrBlk(pclient, pdata, len);
	    len -= BLOCK_LEN;
	    pdata += BLOCK_LEN;
	} while ((status == DN_SUCCESS) && (len > 0));
    }
    dnpEOT(pclient);
    return status;
}

static int dnpRead(dnAsynClient *pclient, int cmd, int addr, char *pdata, int len) {
    int status;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpRead(%p, %d, %d, %p, %d)\n", pclient, cmd, addr, pdata, len);
    
    status = dnpHeader(pclient, cmd, addr, len);
    if (status == DN_SUCCESS) {
	do {
	    status = dnpRdBlk(pclient, pdata, len);
	    len -= BLOCK_LEN;
	    pdata += BLOCK_LEN;
	} while ((status == DN_SUCCESS) && (len > 0));
    }
    if ((status == DN_SUCCESS) &&
	(dnpGetc(pclient) != EOTCHAR)) status = DN_NOT_EOT;
    dnpEOT(pclient);
    return status;
}


/* Asyn callback routines */

static void dncQueueCallback(asynUser *pau) {
    struct plcMessage* pMsg = (struct plcMessage*) pau->userPvt;
    dnAsynClient *pclient = pMsg->pClient;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dncQueueCallback(%p)\n", pau);
    
    if (pMsg->cmd & WRITECMD) {
	pMsg->status = dnpWrite(pclient, pMsg->cmd, pMsg->addr, pMsg->pdata, pMsg->len);
    } else {
	pMsg->status = dnpRead(pclient, pMsg->cmd, pMsg->addr, pMsg->pdata, pMsg->len);
    }
    pMsg->callback(pMsg);
}

static void dncQueueTimeout(asynUser *pau) {
    struct plcMessage* pMsg = (struct plcMessage*) pau->userPvt;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dncQueueTimeout(%p)\n", pau);
    
    pMsg->status = DN_TIMEOUT;
    pMsg->callback(pMsg);

}

static void dncException(asynUser *pau, asynException why) {
    struct plcMessage* pMsg = (struct plcMessage*) pau->userPvt;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dncException(%p)\n", pau);
    
    if (why == asynExceptionConnect && pMsg->connstat) {
	int connected;
	pasynManager->isConnected(pau, &connected);
	pMsg->connstat(pMsg, connected);
    }
}

/* Exported routines */

int epicsShareAPI initDnAsynClient(struct plcMessage* pMsg) {
    dnAsynClient *pclient;
    asynUser *pau;
    asynStatus status;
    asynInterface *pif;
    
    pclient = (dnAsynClient *) calloc(1, sizeof(dnAsynClient));
    if (pclient == NULL) {
	errlogPrintf("initDnAsynClient: calloc failed\n");
	goto err_return;
    }
    
    pau = pasynManager->createAsynUser(dncQueueCallback, dncQueueTimeout);
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "initDnAsynClient(%p)\n", pMsg);
    pau->userPvt = pMsg;
    pclient->pau = pau;
    
    status = pasynManager->connectDevice(pau, pMsg->port, 0);
    if (status != asynSuccess) {
	errlogPrintf("initDnAsynClient: Can't connect to Asyn port \"%s\":\n\t%s \n",
		     pMsg->port, pau->errorMessage);
	goto err_freeAsynUser;
    }
    
    pif = pasynManager->findInterface(pau, asynOctetType, 1);
    if (pif == NULL) {
	errlogPrintf("initDnAsynClient: %s interface not supported by Asyn port \"%s\"\n",
		     asynOctetType, pMsg->port);
	goto err_disconnect;
    }
    pclient->poctet = (asynOctet *) pif->pinterface;
    pclient->drvPvt = pif->drvPvt;
    
    status = pasynManager->exceptionCallbackAdd(pau, dncException);
    if (status != asynSuccess) {
	errlogPrintf("initDnAsynClient: Can't add exception handler for Asyn port \"%s\":\n\t%s \n",
		     pMsg->port, pau->errorMessage);
	/* Not a severe error, so don't give up */
    }
    
    pMsg->pClient = pclient;
    return 0;

err_disconnect:
    pasynManager->disconnect(pau);
err_freeAsynUser:
    pasynManager->freeAsynUser(pau);
    free(pclient);
err_return:
    return -1;
}

int epicsShareAPI dnAsynClientSend(struct plcMessage *pMsg) {
    dnAsynClient *pclient = pMsg->pClient;
    asynUser *pau = pclient->pau;
    asynStatus status;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnAsynClientSend(%p)\n", pMsg);
    
    pMsg->status = DN_INTERNAL;
    
    status = pasynManager->queueRequest(pau, asynQueuePriorityMedium, 20.0);
    return status;
}
