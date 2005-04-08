/******************************************************************************

Project:
    DirectNet ASYN

File:
    devXiDnAsyn.c

Description:
    Input device support routines for directNet over ASYN

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include <recGbl.h>
#include <recSup.h>
#include <devSup.h>
#include <dbScan.h>
#include <drvSup.h>
#include <errlog.h>
#include <dbLock.h>
#include <alarm.h>
#include <epicsMutex.h>
#include <epicsExport.h>

#include <aiRecord.h>
#include <biRecord.h>
#include <mbbiRecord.h>
#include <mbbiDirectRecord.h>
#include <menuScan.h>

#include "directNetClient.h"
#include "directNetAsyn.h"
#include "devDnAsyn.h"


struct rdCache {
    struct rdCache *pNext;
    struct rdItem {
	struct plcMessage msg;		/* *MUST* be first, see devXiDnCallback */
	epicsMutexId msgMutex;		/* Protects msgData .. active */
	char msgData[DN_RDDATA_MAX];	/* This is the I/O buffer */
	unsigned int startAddr;
	unsigned char nWords;
	unsigned char active;
	epicsMutexId cacheMutex;	/* Protects data .. timestamp */
	unsigned short data[DN_RDDATA_MAX / DN_PLCWORDLEN];	/* data cache */
	epicsTimeStamp timestamp;
	IOSCANPVT intInfo;
	struct dpvtIn *recList;
    } item;
};

struct dpvtIn {
    struct dbCommon *precord;
    enum recType {AI, BI, MBBI, MBBID} type;
    unsigned char waiting;
    struct plcAddr plcAddr;
    struct plcInfo *plcInfo;
    struct rdItem *rdItem;
    struct dpvtIn *recNext;
    epicsTimeStamp start;
};

static const char *recTypeName[] = {
    "ai", "bi", "mbbi", "mbbiDirect"
};

static void ioReport(int detail, struct plcInfo *pPlc) {
    struct rdCache *pcache = pPlc->rdCache;
    
    while (pcache) {
	switch (detail) {
	case 2: {
	    char when[64];
	    epicsTimeToStrftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S.%06f",
				&pcache->item.timestamp);
	    printf("    RdCache for V%o - V%o last updated at %s\n",
		   pcache->item.startAddr - DNREFOFFSET,
		   pcache->item.startAddr - DNREFOFFSET + pcache->item.nWords - 1,
		   when);
	    }
	    break;
	    
	case 3: {
		int i;
		
		printf("    RdCache buffer for V%o - V%o holds (hex):\n       ",
			pcache->item.startAddr - DNREFOFFSET,
			pcache->item.startAddr - DNREFOFFSET + pcache->item.nWords - 1);
		for (i=0; i < pcache->item.nWords; i++) {
		    printf(" %04x", pcache->item.data[i]);
		}
		putchar('\n');
	    }
	    break;
	    
	case 4: {
		struct dpvtIn *dpvt = pcache->item.recList;
		
		printf("    RdCache buffer for V%o - V%o is used by:\n       ",
			pcache->item.startAddr - DNREFOFFSET,
			pcache->item.startAddr - DNREFOFFSET + pcache->item.nWords - 1);
		while (dpvt) {
		    printf("	    %s (%s)",
			    dpvt->precord->name, recTypeName[dpvt->type]);
		    if (dpvt->plcAddr.bitNum)
			printf(" at B%o.%o\n",
			       dpvt->plcAddr.vAddr - DNREFOFFSET,
			       dpvt->plcAddr.bitNum);
		    else
			printf(" at V%o\n",
			       dpvt->plcAddr.vAddr - DNREFOFFSET);
		    
		    dpvt = dpvt->recNext;
		}
	    }
	    break;
	}
	pcache = pcache->pNext;
    }
    
    return;
}

static long report(int detail) {
    if (detail > 1) dnAsynReport(detail, ioReport);
    return 0;
}


static void devXiDnConnstat(struct plcMessage *pMsg, int connected) {
    /* This uses a kludge, we actually need the address of the struct rdItem.
     * The two must be identical or this will fail. */
    struct rdItem *pitem = (struct rdItem *) pMsg;
    struct dpvtIn *dpvt = pitem->recList;
    struct plcInfo *pPlc = dpvt->plcInfo;
    
    errlogPrintf("devDnAsyn: Asyn port \"%s\" %sconnected (PLC \"%s\")\n", 
		 pMsg->port, connected ? "" : "dis", pPlc->name);
}

static void devXiDnCallback(struct plcMessage *pMsg) {
    /* This uses a kludge, we actually need the address of the struct rdItem.
     * The two must be identical or this will fail. */
    struct rdItem *pitem = (struct rdItem *) pMsg;
    struct dpvtIn *dpvt = pitem->recList;
    struct plcInfo *pPlc = dpvt->plcInfo;
    int i;
    
    if (devDnAsynDebug >= 10) {
	printf("devXiDnAsyn: Got a reply from PLC \"%s\"\n", 
		pPlc->name);
	printf("devXiDnAsyn: Read V%o[%d] returned status %d\n",
		pMsg->addr - DNREFOFFSET, pMsg->len, pMsg->status);
    } else if (pMsg->status && devDnAsynDebug >= 5)
	printf("devXiDnAsyn: Read reply from PLC \"%s\" has status %d\n",
		pPlc->name, pMsg->status);
    
    if (pMsg->status == DN_SUCCESS) {
	/* Read succeeded */
	pPlc->alarm = NO_ALARM;
	pPlc->nSuccess++;
    } else if (pMsg->status > DN_TIMEOUT) {
	/* DirectNet I/O problem */
	errlogPrintf("devXiDnAsyn: DirectNet error %s from PLC \"%s\" on Asyn port \"%s\"\n",
		     dn_error_strings[pMsg->status], pPlc->name, pMsg->port);
	pPlc->alarm = INVALID_ALARM;
	pPlc->nDnFail++;
    } else {
	/* ASYN I/O problem */
	errlogPrintf("devXiDnAsyn: dnAsyn error %s from port \"%s\" (PLC \"%s\")\n",
		     dn_error_strings[pMsg->status], pMsg->port, pPlc->name);
	pPlc->alarm = MAJOR_ALARM;
	pPlc->nAsynFail++;
    }
    
    if (devDnAsynDebug >= 10 && pPlc->alarm == NO_ALARM) {
	printf("devXiDnAsyn: Got read reply: cmd=%#x addr=%#o, len=%d, status=%d",
	       pMsg->cmd, pMsg->addr - DNREFOFFSET, pMsg->len, pMsg->status);
	for (i=0; i < pMsg->len; i++) {
	    printf("\ndevXiDnAsyn: read data[%d] = ", i);
	    while ((i < pMsg->len) && ((i+1) & 7))
		printf ("%2.2x ", 0xff & pMsg->pdata[i++]);
	}
	printf("\n");
    }

    epicsMutexMustLock(pitem->msgMutex);
    epicsMutexMustLock(pitem->cacheMutex);
    if (pPlc->alarm == NO_ALARM) {
	/* Cache the reply data */
	for (i=0; i < pitem->nWords; i++) {
	    pitem->data[i] = ((0xff & pMsg->pdata[2*i+1]) << 8) |
			      (0xff & pMsg->pdata[2*i]);
	}
    }
    /* Update timestamp even on error so I/O Intr records don't retry I/O */
    epicsTimeGetCurrent(&pitem->timestamp);
    epicsMutexUnlock(pitem->cacheMutex);
    pitem->active = FALSE;
    epicsMutexUnlock(pitem->msgMutex);
    
    /* Now process all the waiting records */
    while (dpvt != NULL) {
	struct dbCommon *prec = dpvt->precord;
	struct rset *prset = prec->rset;
	if (devDnAsynDebug >= 15) {
	    printf("Examining \"%s\", waiting = %d\n", prec->name, dpvt->waiting);
	}
	if (dpvt->waiting) {
	    dbScanLock(dpvt->precord);
	    (*prset->process)(dpvt->precord);
	    dbScanUnlock(dpvt->precord);
	}
	dpvt = dpvt->recNext;
    }
    
    /* Finally trigger any I/O Interrupt records */
    scanIoRequest(pitem->intInfo);
}


static long init_input(struct dbCommon *prec, enum recType type, struct link *plink) {
    struct dpvtIn *dpvt;
    struct rdCache **ppcache, *pcache;
    struct plcInfo *pPlc;
    unsigned int addr;
    long status;
    const int maxWords = DN_RDDATA_MAX / DN_PLCWORDLEN;
    
    if (devDnAsynDebug > 0)
	printf ("devXiDnAsyn: init_input invoked for \"%s\"\n", prec->name);
    
    dpvt = (struct dpvtIn *) calloc(1, sizeof(struct dpvtIn));
    if (dpvt == NULL) {
	errlogPrintf("devXiDnAsyn: calloc failed for \"%s\"\n", prec->name);
	prec->pact = TRUE;
	return S_rec_outMem;
    }
    prec->dpvt = (void *) dpvt;
    
    dpvt->precord = prec;
    dpvt->type    = type;
    dpvt->waiting = FALSE;
    
    status = dnAsynAddr(prec, &dpvt->plcAddr, plink);
    if (status) {
	prec->pact = TRUE;
	return status;
    }
    pPlc = dpvt->plcAddr.plcInfo;
    addr = dpvt->plcAddr.vAddr;
    
    dpvt->plcInfo = pPlc;
    
    if (devDnAsynDebug > 10)
	printf ("devXiDnAsyn: dpvt = %p, vAddr = V%o\n", 
		dpvt, addr - DNREFOFFSET);

    /* Search for an existing cache entry or one that can be extended */
    ppcache = &(pPlc->rdCache);
    pcache = NULL;
    while (*ppcache) {
	if ((addr >= (*ppcache)->item.startAddr + (*ppcache)->item.nWords - maxWords) &&
	    ((*ppcache)->item.startAddr + maxWords > addr)) {
	    if (devDnAsynDebug > 10)
		printf ("devXiDnAsyn: Suitable rdCache entry found: V%o\n", 
			(*ppcache)->item.startAddr - DNREFOFFSET);
	    pcache = *ppcache;
	    break;
	}
	ppcache = &(*ppcache)->pNext;
    }
    
    if (pcache == NULL) {
	struct plcMessage *pMsg;
	
	/* Not found, create a new entry */
	pcache = (struct rdCache *) calloc(1, sizeof (struct rdCache));
	if (pcache == NULL) {
	    errlogPrintf("devXiDnAsyn: calloc failed for \"%s\"\n", prec->name);
	    prec->pact = TRUE;
	    return S_rec_outMem;
	    /* Really ought to free dpvt first... */
	}
	
	if (devDnAsynDebug > 10)
	    printf ("devXiDnAsyn: new rdCache entry created for V%o\n", 
		    addr - DNREFOFFSET);
	
	/* Initialise: cache entry */
	pcache->item.startAddr = addr;
	pcache->item.nWords = 1;
	pcache->item.active = FALSE;
	pcache->item.msgMutex = epicsMutexMustCreate();
	pcache->item.cacheMutex = epicsMutexMustCreate();
	pcache->item.timestamp.secPastEpoch = 0;
	scanIoInit(&pcache->item.intInfo);
	
	/* plcMessage entry */
	pMsg = &pcache->item.msg;
	pMsg->port     = pPlc->port;
	pMsg->cmd      = (pPlc->slaveId << 8) | READVMEM;
	pMsg->len      = DN_PLCWORDLEN;
	pMsg->addr     = addr;
	pMsg->pdata    = pcache->item.msgData;
	pMsg->callback = devXiDnCallback;
	if (!pPlc->connflag) {
	    pMsg->connstat = devXiDnConnstat;
	    pPlc->connflag = TRUE;
	}
	
	/* creat a new ASYN client object */
	status = initDnAsynClient(pMsg);
	if (status) {
	    prec->pact = TRUE;
	    return status;
	}
	
	/* Finally install the various list links */
	dpvt->recNext = NULL;
	pcache->pNext = NULL;
	pcache->item.recList = dpvt;
	(*ppcache) = pcache;
	
    } else if (addr < pcache->item.startAddr) {
	/* Need to extend the existing entry backwards */
	if (devDnAsynDebug > 10)
	    printf ("devXiDnAsyn: Extending entry back to V%o\n", 
		    addr - DNREFOFFSET);
	
	/* Adjust entry start address and length */
	pcache->item.nWords += pcache->item.startAddr - addr;
	pcache->item.startAddr = addr;
	
	/* Repeat that change in the plcMessage */
	pcache->item.msg.addr = dpvt->plcAddr.vAddr;
	pcache->item.msg.len  = pcache->item.nWords * PLCWORDBYTES;
	
	/* Finally insert this record in item's record list */
	dpvt->recNext = pcache->item.recList;
	pcache->item.recList = dpvt;
	
    } else if (addr >= pcache->item.startAddr + pcache->item.nWords) {
	/* Need to extend the existing entry forwards */
	if (devDnAsynDebug > 10)
	    printf ("devXiDnAsyn: Extending entry forward to V%o\n", 
		    addr - DNREFOFFSET);
	
	/* Adjust entry length, in Bitbus message too */
	pcache->item.nWords  = addr - pcache->item.startAddr + 1;
	pcache->item.msg.len = pcache->item.nWords * PLCWORDBYTES;
	
	/* Finally insert this record in item's record list */
	dpvt->recNext = pcache->item.recList;
	pcache->item.recList = dpvt;
	
    } else {
	if (devDnAsynDebug > 10)
	    printf ("devXiDnAsyn: No change needed for V%o\n", 
		    addr - DNREFOFFSET);
	
	/* Just need to insert this record in item's record list */
	dpvt->recNext = pcache->item.recList;
	pcache->item.recList = dpvt;
    }
    
    /* Make sure we can get to the cache line from the record */
    dpvt->rdItem = &pcache->item;
    return 0;
}


static long init_ai(struct aiRecord *prec) {
    long status;

    if (devDnAsynDebug > 0)
	printf ("devXiDnAsyn: Init ai called for \"%s\"\n", prec->name);

    status = init_input((struct dbCommon *) prec, AI, &prec->inp);
    if (status)
	return status;
    
    /* set linear conversion slope & offset, 12 bits assumed (WRONG?) */
    prec->eslo = (prec->eguf - prec->egul)/4095.0;

    return 0;
}


static long init_bi(struct biRecord *prec) {
    long status;
    
    if (devDnAsynDebug > 0)
       printf ("devXiDnAsyn: Init bi called for \"%s\"\n", prec->name);
 
    status = init_input((struct dbCommon *) prec, BI, &prec->inp);
    if (status)
	return status;
    
    prec->mask = 1 << ((struct dpvtIn *)prec->dpvt)->plcAddr.bitNum;
    return 0;
}


static long init_mbbi(struct mbbiRecord *prec) {
    long status;

    if (devDnAsynDebug > 0)
	printf ("devXiDnAsyn: Init mbbi called for \"%s\"\n", prec->name);
    
    status = init_input((struct dbCommon *) prec, MBBI, &prec->inp);
    if (status)
	return status;
    
    prec->shft = ((struct dpvtIn *)prec->dpvt)->plcAddr.bitNum;
    prec->mask <<= prec->shft;
    return 0;
}


static long init_mbbid(struct mbbiDirectRecord *prec) {
    long status;

    if (devDnAsynDebug > 0)
	printf ("devXiDnAsyn: Init mbbiDirect called for \"%s\"\n", prec->name);
    
    status = init_input((struct dbCommon *) prec, MBBID, &prec->inp);
    if (status)
	return status;
    
    prec->shft = ((struct dpvtIn *)prec->dpvt)->plcAddr.bitNum;
    prec->mask <<= prec->shft;
    return 0;
}


static long get_ioint(int cmd, struct dbCommon *prec, IOSCANPVT *ppvt) {
    struct dpvtIn *dpvt=(struct dpvtIn *)prec->dpvt;
    struct rdItem *pitem = dpvt->rdItem;
    
    if (devDnAsynDebug >= 35)
	printf ("devXiDnAsyn: get_ioint called for \"%s\"\n", prec->name);
    
    *ppvt = pitem->intInfo;
    return 0;
}



static void get_data(struct dbCommon *prec) {
    struct dpvtIn *dpvt=(struct dpvtIn *)prec->dpvt;
    struct rdItem *pitem = dpvt->rdItem;
    unsigned long value;
    
    if (devDnAsynDebug >= 35)
	printf ("devXiDnAsyn: get_data called for \"%s\"\n", prec->name);
    
    /* Deal with alarms first */
    if (dpvt->plcInfo->alarm) {
	if (devDnAsynDebug >= 35)
	    printf ("devXiDnAsyn: alarm status %d\n", dpvt->plcInfo->alarm);
	recGblSetSevr(prec, READ_ALARM, dpvt->plcInfo->alarm);
	return;
    }
    
    /* Find our particular number */
    value = pitem->data[dpvt->plcAddr.vAddr - pitem->startAddr];

    if (devDnAsynDebug >= 35)
	printf ("devXiDnAsyn: Raw value = %#lx\n", value);

    switch(dpvt->type) {
	struct biRecord *bi;
	struct aiRecord *ai;
	struct mbbiRecord *mbbi;
	struct mbbiDirectRecord *mbbid;

	case AI:
	    ai=(struct aiRecord *)prec;
	    ai->rval = value;
	    break;

	case BI:
	    bi = (struct biRecord *)prec;
	    bi->rval = value & bi->mask;
	    break;

	case MBBI:
	    mbbi = (struct mbbiRecord *)prec;
	    mbbi->rval = value & mbbi->mask;
	    break;

	case MBBID:
	    mbbid = (struct mbbiDirectRecord *)prec;
	    mbbid->rval = value & mbbid->mask;
	    break;
    }
    return;
}


static long read_data(struct dbCommon *prec) {
    struct dpvtIn *dpvt = (struct dpvtIn *) prec->dpvt;
    struct rdItem *pitem;
    struct plcInfo *pPlc;

    if (devDnAsynDebug >= 35)
       printf ("devXiDnAsyn: read_data called for \"%s\"\n", prec->name);

    if (!dpvt) return S_dev_NoInit;
    
    pitem = dpvt->rdItem;
    pPlc = dpvt->plcInfo;
    
    if (!prec->pact) {
	/* This is a read request, check the cache */
	epicsTimeStamp tNow;
	double staleTime;
	switch (prec->scan) {
	case menuScanPassive:
	    staleTime = 0.1;
	    break;
	case menuScanEvent:
	    staleTime = 0.1;
	    break;
	case menuScanI_O_Intr:
	    staleTime = 10.0;
	    break;
	default:
	    staleTime = scanPeriod(prec->scan) / 2;
	}
	epicsTimeGetCurrent(&tNow);
	
	/* If the cached data is not stale ... */
	epicsMutexMustLock(pitem->cacheMutex);
	if (epicsTimeDiffInSeconds(&tNow, &pitem->timestamp) < staleTime) {
	    /* .. then we can use it */
	    if (devDnAsynDebug >= 3)
		printf("devXiDnAsyn: Using value from read cache\n");
	    
	    get_data(prec);
	    epicsMutexUnlock(pitem->cacheMutex);
	    return 0;
	}
	epicsMutexUnlock(pitem->cacheMutex);
	/* Can't use cache data, must request a read */
	
	if (devDnAsynDebug >= 3)
	    epicsTimeGetCurrent(&dpvt->start);
	dpvt->waiting = TRUE;
	
	/* Send the request */
	epicsMutexMustLock(pitem->msgMutex);
	if (!pitem->active) {
	    pitem->active = TRUE;
	    if (dnAsynClientSend(&pitem->msg)) {
		pitem->active = FALSE;
		epicsMutexUnlock(pitem->msgMutex);
		recGblSetSevr(prec, WRITE_ALARM, MAJOR_ALARM);
		errlogPrintf("devXiDnAsyn: ASYN Send by \"%s\" failed\n", prec->name);
		pPlc->nAsynFail++;
		return -1;
	    }
	    pPlc->nRdReqs++;
	}
	epicsMutexUnlock(pitem->msgMutex);
	
	if (devDnAsynDebug >= 10)
	    printf("devXiDnAsyn: Read requested at V%o\n", 
		   pitem->startAddr - DNREFOFFSET);
	
	prec->pact = TRUE;
    } else {
	/* An ASYN request has completed */
	if (devDnAsynDebug >= 5)
	    printf("devXiDnAsyn: alarm = %d\n", pPlc->alarm);
	
	epicsMutexMustLock(pitem->cacheMutex);
	get_data(prec);
	epicsMutexUnlock(pitem->cacheMutex);
	
	dpvt->waiting = FALSE;
	if (devDnAsynDebug >= 3) {
	    epicsTimeStamp tNow;
	    double duration;
	    epicsTimeGetCurrent(&tNow);
	    duration = epicsTimeDiffInSeconds(&tNow, &dpvt->start);
	    printf("devXiDnAsyn: Record processing for \"%s\" took %f seconds\n", 
		    prec->name, duration);
	}
    }

    return 0;
}


/* Device Support Entry Tables */

XXDSET devAiDnAsyn    = { 6,NULL,  NULL,init_ai,   get_ioint,read_data, NULL };
XXDSET devBiDnAsyn    = { 5,report,NULL,init_bi,   get_ioint,read_data };
XXDSET devMbbiDnAsyn  = { 5,NULL,  NULL,init_mbbi, get_ioint,read_data };
XXDSET devMbbidDnAsyn = { 5,NULL,  NULL,init_mbbid,get_ioint,read_data };

epicsExportAddress(dset, devAiDnAsyn);
epicsExportAddress(dset, devBiDnAsyn);
epicsExportAddress(dset, devMbbiDnAsyn);
epicsExportAddress(dset, devMbbidDnAsyn);
