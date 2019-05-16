/******************************************************************************

Project:
    DirectNet ASYN

File:
    directNetClient.c

Description:
    Client support routines for directNet or simulator over ASYN

Author:
    Andrew Johnson

******************************************************************************/

/* OS */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* libCom */
#include <errlog.h>
#include <epicsTime.h>

/* asyn */
#include <asynDriver.h>
#include <asynOctet.h>

#define epicsExportSharedSymbols

/* directNetAsyn */
#include "directNetAsyn.h"
#include "directNetClient.h"

/* Packet interface methods */

typedef struct dnAsynClient dnAsynClient;

typedef struct plcProto {
    int (*read)(dnAsynClient *pclient,
        int cmd, int addr, char *pdata, int len);
    int (*write)(dnAsynClient *pclient,
        int cmd, int addr, const char *pdata, int len);
} plcProto;

/* Client data structure */

struct dnAsynClient {
    asynUser *pau;
    asynOctet *poctet;
    void *drvPvt;
};


const char *dn_error_strings[] = {
    "DN_SUCCESS",
    "DN_INTERNAL",
    "DN_TIMEOUT",
    "DN_SEND_FAIL",
    "DN_SEL_FAIL",
    "DN_HDR_FAIL",
    "DN_RDBLK_FAIL",
    "DN_WRBLK_FAIL",
    "DN_NOT_EOT",
    "DN_GOT_EOT"
};

int dnAsynMaxRetries = MAX_RETRIES;


/* asynOctet interface routines */

static void dnpSend(dnAsynClient *pclient, const char *pdata, int len) {
    asynUser *pau = pclient->pau;
    asynStatus status;
    size_t wrote;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpSend(%p, %p, %d)\n", pclient, pdata, len);
    
    status = pclient->poctet->write(pclient->drvPvt, pau, pdata, len, &wrote);
    if (status == asynSuccess) {
	asynPrintIO(pau, ASYN_TRACEIO_DEVICE, pdata, len,
		    "dnpSend: sent %lu of %d bytes\n",
                    (unsigned long) wrote, len);
    } else {
	asynPrint(pau, ASYN_TRACE_ERROR,
		    "dnpSend: write failed: %s\n", pau->errorMessage);
    }
}

static int dnpGets(dnAsynClient *pclient, char *pdata, int len) {
    asynUser *pau = pclient->pau;
    asynStatus status;
    size_t got;
    int why;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpGets(%p, %p, %d)\n", pclient, pdata, len);
    
    status = pclient->poctet->read(pclient->drvPvt, pau, pdata, len, &got, &why);
    if (status == asynSuccess) {
	int trace = ASYN_TRACEIO_DEVICE;
	int retval = 0;
	if (got != len) {
	    trace |= ASYN_TRACE_ERROR;
	    retval = -1;
	}
	asynPrintIO(pau, trace, pdata, got,
		    "dnpGets: Got %lu of %d bytes, reason 0x%x\n",
		    (unsigned long) got, len, why);
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
    unsigned char reply[1]; /* 0..255 */
    int result;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpGetc(%p)\n", pclient);
    
    result = dnpGets(pclient, (char *) reply, 1);
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


/* DirectNet protocol implementation */

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
    int retries = dnAsynMaxRetries;
    int slaveId = target + SLAVEOFFSET;
    int sendlen = 3;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpSelect(%p, %d)\n", pclient, target);
    
    reselect[0] = EOTCHAR;
    reselect[1] = SEQCHAR;
    reselect[2] = slaveId;
    reselect[3] = ENQCHAR;
    pau->timeout = ENQACKDELAY + 4 / BYTERATE;
    do {
	int reply = dnpSendGetc(pclient, select, sendlen);
	while (reply > 0 && reply != SEQCHAR && reply != EOTCHAR) {
	    asynPrint(pau, ASYN_TRACE_ERROR,
		      "dnpSelect: Not SEQ/EOT - %d (retries = %d)\n",
		      reply, retries);
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
    int reply, retries = dnAsynMaxRetries;
    char header[HEADER_LEN];
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpHeader(%p, %d, %d, %d)\n", pclient, cmd, addr, len);
    
    reply = dnpSelect(pclient, (cmd >> 8) & 0xff);
    if (reply) return reply;
    
    sprintf((char *) header, "%c%4.4X%4.4X%4.4X%2.2X%c", 
	    SOHCHAR, cmd, addr, len, MASTERID, ETBCHAR);
    header[HEADER_LEN-1] = dnpLRC(pclient, &header[1], HEADER_LEN - 3);
    pau->timeout = HDRACKDELAY + HEADER_LEN / BYTERATE;

    do {
	reply = dnpSendGetc(pclient, header, HEADER_LEN);
	if (reply == ACKCHAR) return DN_SUCCESS;
	if (reply == EOTCHAR) return DN_GOT_EOT;
    } while (reply == NAKCHAR && --retries > 0);
    if (reply != NAKCHAR)
	asynPrint(pau, ASYN_TRACE_ERROR,
		  "dnpHeader: Not ACK/NAK/EOT - %d (retries = %d)\n",
		  reply, retries);
    return DN_HDR_FAIL;
}

static int dnpWrBlk(dnAsynClient *pclient, const char *pdata, int len) {
    asynUser *pau = pclient->pau;
    char block[BLOCK_LEN + 3];
    int blen = len;
    int reply, retries = dnAsynMaxRetries;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpWrBlk(%p, %p, %d)\n", pclient, pdata, len);
    
    if (blen > BLOCK_LEN) blen = BLOCK_LEN;
    len -= blen;
    
    block[0] = STXCHAR;
    memcpy(&block[1], pdata, blen);
    block[blen+1] = len ? ETBCHAR : ETXCHAR;
    block[blen+2] = dnpLRC(pclient, &block[1], blen);
    pau->timeout = DATACKDELAY + (BLOCK_LEN + 3) / BYTERATE;
    do {
	reply = dnpSendGetc(pclient, block, blen + 3);
	if (reply == ACKCHAR) return DN_SUCCESS;
    } while (reply == NAKCHAR && --retries > 0);
    return DN_WRBLK_FAIL;
}

static int dnpRdBlk(dnAsynClient *pclient, char *pdata, int len) {
    asynUser *pau = pclient->pau;
    int blen = len;
    int reply, retries = dnAsynMaxRetries;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnpRdBlk(%p, %p, %d)\n", pclient, pdata, len);
    
    if (blen > BLOCK_LEN) blen = BLOCK_LEN;
    len -= blen;
    pau->timeout = DATACKDELAY + BLOCK_LEN / BYTERATE;
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
	    dnpSend(pclient, &ack, 1);
	    return DN_SUCCESS;
	}
	
	dnpSend(pclient, &nak, 1);
    } while (--retries > 0);
    return DN_RDBLK_FAIL;
}

static void dnpEOT(dnAsynClient *pclient) {
    char data[] = {EOTCHAR};
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpEOT(%p)\n", pclient);
    
    dnpSend(pclient, data, 1);
}


/* Protocol interface routines for directNet */

static int dnpWrite(dnAsynClient *pclient, int cmd, int addr, const char *pdata, int len) {
    int status, retries = dnAsynMaxRetries;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpWrite(%p)\n", pclient);
    do {
	status = dnpHeader(pclient, cmd, addr, len);
	if (status == DN_SUCCESS) {
	    do {
		status = dnpWrBlk(pclient, pdata, len);
		len -= BLOCK_LEN;
		pdata += BLOCK_LEN;
	    } while ((status == DN_SUCCESS) && (len > 0));
	}
	dnpEOT(pclient);
    } while ((status == DN_GOT_EOT) && (--retries > 0));
    return status;
}

static int dnpRead(dnAsynClient *pclient, int cmd, int addr, char *pdata, int len) {
    int status, retries = dnAsynMaxRetries;
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
	      "dnpRead(%p, %d, %d, %p, %d)\n", pclient, cmd, addr, pdata, len);
    do {
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
    } while ((status == DN_GOT_EOT) && (--retries > 0));
    return status;
}

const plcProto dnpProto = {
    dnpRead, dnpWrite
};


/* Simulator protocol implementation */

static void simCommand(dnAsynClient *pclient, int cmd, int addr, int len) {
    asynUser *pau = pclient->pau;
    int id = (cmd >> 8) & 0xff;
    char msg = (cmd & WRITECMD) ? 'W' : 'R';
    char command[2+3+3+5+5+1+1]; /* <m:1> <id:2> <cmd:2> <addr:4> <len:4> */

    asynPrint(pau, ASYN_TRACE_FLOW,
        "simCommand(%p, %d, %d, %d)\n", pclient, cmd, addr, len);

    /* Encode the command as ASCII */
    sprintf(command, "%c %2.2x %2.2x %4.4x %4.4x\n",
        msg, id, cmd & 0xff, addr, len);
    dnpSend(pclient, command, strlen(command));
}

static void simWriteMsg(dnAsynClient *pclient, const char *pdata, int len) {
    asynUser *pau = pclient->pau;
    char *next;
    char block[2+3+32*2+1+1]; /* D <len:2> <d00:2><d01:2>...<d1f:2> */

    asynPrint(pau, ASYN_TRACE_FLOW,
        "simWriteMsg(%p, %p, %d)\n", pclient, pdata, len);

    if (len > 32)
        len = 32;

    next = block + sprintf(block, "D %2.2x ", len);
    while (len-- > 0) {
        next += sprintf(next, "%2.2x", (unsigned) *pdata++);
    }
    *next++ = '\n';

    dnpSend(pclient, block, next - block);
}

static int simResponse(dnAsynClient *pclient) {
    asynUser *pau = pclient->pau;
    epicsTimeStamp T_end;
    char buffer[80];
    size_t len;
    int reply, ch;

    asynPrint(pau, ASYN_TRACE_FLOW,
        "simResponse(%p)\n", pclient);

    epicsTimeGetCurrent(&T_end);
    epicsTimeAddSeconds(&T_end, pau->timeout);

    do {
        epicsTimeStamp T_now;

        reply = dnpGetc(pclient);
        epicsTimeGetCurrent(&T_now);
        pau->timeout = epicsTimeDiffInSeconds(&T_end, &T_now);

        if (reply < 0)
            return reply;
    } while (reply == '\n' || reply == '\r');

    switch (reply) {
    case 'N':
        len = 0;
        do {
            epicsTimeStamp T_now;

            ch = dnpGetc(pclient);
            epicsTimeGetCurrent(&T_now);
            pau->timeout = epicsTimeDiffInSeconds(&T_end, &T_now);

            if (ch < 0)
                break;
                /* A timeout here terminates the NAK message
                 * but is otherwise ignored.
                 */

            if (isprint(ch)) {
                buffer[len] = ch;
                if (len+1 < sizeof(buffer))
                    len++;
            }
        } while (ch != '\n' && ch != '\r');
        buffer[len] = 0;

        asynPrint(pau, ASYN_TRACE_ERROR,
            "simResponse: Recived NAK '%s'\n", buffer);

        /* Fall through... */
    case 'A':
    case 'X':
    case 'D':
        return reply;

    default:
        asynPrint(pau, ASYN_TRACE_ERROR,
            "simResponse: Unknown response %d ('%c')\n",
            reply, isprint(reply) ? reply : ' ');
        return -1;
    }
}

static int simWriteData(dnAsynClient *pclient, const char *pdata, int len) {
    asynUser *pau = pclient->pau;
    int status;

    asynPrint(pau, ASYN_TRACE_FLOW,
              "simWriteData(%p, %p, %d)\n", pclient, pdata, len);

    do {
        simWriteMsg(pclient, pdata, len);
        len -= 32;
        pdata += 32;
    } while (len > 0);

    status = simResponse(pclient);
    if (status == 'A')
        return DN_SUCCESS;

    return DN_WRBLK_FAIL;
}

static int simReadByte(dnAsynClient *pclient) {
    asynUser *pau = pclient->pau;
    int ch, val;

    do {
        ch = dnpGetc(pclient);
    } while (isspace(ch));

    if (isxdigit(ch)) {
        if (ch >= 'a')
            val = ch + 10 - 'a' ;
        else if (ch >= 'A')
            val = ch + 10 - 'A' ;
        else
            val = ch - '0';
        ch = dnpGetc(pclient);
        if (isxdigit(ch)) {
            val <<= 4;
            if (ch >= 'a')
                val += ch + 10 - 'a' ;
            else if (ch >= 'A')
                val += ch + 10 - 'A' ;
            else
                val += ch - '0';
        }
        else
            val = -1;
    }
    else
        val = -1;

    if (val < 0)
        asynPrint(pau, ASYN_TRACE_ERROR,
            "simReadByte: Expected hex character: '%c'\n", ch);

    return val;
}

static int simReadMsg(dnAsynClient *pclient, char *pdata, int len, int *got) {
    asynUser *pau = pclient->pau;
    epicsTimeStamp T_end;
    int reply, blen, rlen, i;

    asynPrint(pau, ASYN_TRACE_FLOW,
        "simReadMsg(%p, %p, %d)\n", pclient, pdata, len);

    epicsTimeGetCurrent(&T_end);
    epicsTimeAddSeconds(&T_end, pau->timeout);

    reply = simResponse(pclient);
    if (reply != 'D')
        return reply;

    blen = simReadByte(pclient);
    if (blen < 0)
        return blen;
    if (blen > len) {
        rlen = len;
        asynPrint(pau, ASYN_TRACE_ERROR,
            "simReadMsg: Requested %d bytes but reply is %d bytes!\n",
            len, blen);
    }
    else
        rlen = blen;

    for (i = 0; i < rlen; i++) {
        int val;
        epicsTimeStamp T_now;

        val = simReadByte(pclient);
        epicsTimeGetCurrent(&T_now);
        pau->timeout = epicsTimeDiffInSeconds(&T_end, &T_now);

        if (val < 0)
            return val;

        *pdata++ = val;
        *got += 1;
    }

    /* Discard any extra data */
    for (; i < blen; i++) {
        int val;
        epicsTimeStamp T_now;

        val = simReadByte(pclient);
        epicsTimeGetCurrent(&T_now);
        pau->timeout = epicsTimeDiffInSeconds(&T_end, &T_now);

        if (val < 0)
            return val;
    }

    return reply;
}

static int simCancelRead(dnAsynClient *pclient) {
    asynUser *pau = pclient->pau;
    epicsTimeStamp T_end;
    int reply;

    asynPrint(pau, ASYN_TRACE_FLOW,
        "simCancelRead(%p)\n", pclient);

    epicsTimeGetCurrent(&T_end);
    epicsTimeAddSeconds(&T_end, pau->timeout);

    dnpSend(pclient, "X\n", 2);
    do {
        epicsTimeStamp T_now;

        epicsTimeGetCurrent(&T_now);
        pau->timeout = epicsTimeDiffInSeconds(&T_end, &T_now);
        if (pau->timeout <= 0)
            return DN_TIMEOUT;

        reply = dnpGetc(pclient);
        if (reply < 0)
            return reply;
    } while (reply != 'A');
    return DN_SUCCESS;
}

static int simReadData(dnAsynClient *pclient, char *pdata, int len) {
    asynUser *pau = pclient->pau;
    epicsTimeStamp T_end;
    int expected = len;

    asynPrint(pau, ASYN_TRACE_FLOW,
        "simReadData(%p, %p, %d)\n", pclient, pdata, len);

    epicsTimeGetCurrent(&T_end);
    epicsTimeAddSeconds(&T_end, pau->timeout);

    do {
        epicsTimeStamp T_now;
        int got = 0;
        int status = simReadMsg(pclient, pdata, expected, &got);

        expected -= got;
        epicsTimeGetCurrent(&T_now);
        pau->timeout = epicsTimeDiffInSeconds(&T_end, &T_now);

        if (status != 'D') {
            if (status == 'N') {
                if (expected == len)
                    asynPrint(pau, ASYN_TRACE_ERROR,
                              "Read request rejected, no data received\n");
                else if (expected > 0)
                    asynPrint(pau, ASYN_TRACE_ERROR,
                              "Partial read, %d bytes missing\n", expected);
            }
            else {
                asynPrint(pau, ASYN_TRACE_ERROR,
                    "simReadData: Unexpected response %d ('%c'), cancelling\n",
                    status, isprint(status) ? status : ' ');
                simCancelRead(pclient);
            }
            return DN_RDBLK_FAIL;
        }
        pdata += got;

        if (pau->timeout <= 0) {
            asynPrint(pau, ASYN_TRACE_ERROR,
                "simReadData: Timeout, cancelling\n");
            simCancelRead(pclient);
            return DN_RDBLK_FAIL;
        }
    } while (expected > 0);

    return DN_SUCCESS;
}


/* Protocol interface routines for simulator */

static int simWrite(dnAsynClient *pclient, int cmd, int addr,
    const char *pdata, int len)
{
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
        "simWrite(%p, %d, %d, %p, %d)\n", pclient, cmd, addr, pdata, len);

    pclient->pau->timeout = 20.0;

    simCommand(pclient, cmd, addr, len);

    return simWriteData(pclient, pdata, len);
}

static int simRead(dnAsynClient *pclient, int cmd, int addr,
    char *pdata, int len)
{
    asynPrint(pclient->pau, ASYN_TRACE_FLOW,
        "simRead(%p, %d, %d, %p, %d)\n", pclient, cmd, addr, pdata, len);

    pclient->pau->timeout = 20.0;

    simCommand(pclient, cmd, addr, len);

    return simReadData(pclient, pdata, len);
}

const plcProto simProto = {
    simRead, simWrite
};


/* Asyn callback routines */

static void dncQueueCallback(asynUser *pau) {
    struct plcMessage* pMsg = (struct plcMessage*) pau->userPvt;
    dnAsynClient *pclient = pMsg->pClient;
    const plcProto *proto = pMsg->proto;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dncQueueCallback(%p)\n", pau);
    
    if (pMsg->cmd & WRITECMD) {
	pMsg->status = proto->write(pclient,
	    pMsg->cmd, pMsg->addr, pMsg->pdata, pMsg->len);
    } else {
	pMsg->status = proto->read(pclient,
	    pMsg->cmd, pMsg->addr, pMsg->pdata, pMsg->len);
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

int initDnAsynClient(struct plcMessage* pMsg) {
    dnAsynClient *pclient;
    asynUser *pau;
    asynStatus status;
    asynInterface *pif;

    if (!pMsg->proto) {
        errlogPrintf("initDnAsynClient: Protocol pointer not set\n");
        goto err_return;
    }

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

int dnAsynClientSend(struct plcMessage *pMsg) {
    dnAsynClient *pclient = pMsg->pClient;
    asynUser *pau = pclient->pau;
    asynStatus status;
    asynPrint(pau, ASYN_TRACE_FLOW,
	      "dnAsynClientSend(%p)\n", pMsg);
    
    pMsg->status = DN_INTERNAL;
    
    status = pasynManager->queueRequest(pau, asynQueuePriorityMedium, 20.0);
    return status;
}
