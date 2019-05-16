/******************************************************************************

Project:
    DirectNet ASYN

File:
    devXoDnAsyn.c

Description:
    Output device support routines for directNet over ASYN

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

/* OS */
#include <stdio.h>
#include <stdlib.h>

/* libCom */
#include <alarm.h>
#include <errlog.h>

/* IOC */
#include <dbLock.h>
#include <devSup.h>
#include <drvSup.h>
#include <recGbl.h>
#include <recSup.h>

/* Records */
#include <aoRecord.h>
#include <boRecord.h>
#include <mbboRecord.h>
#include <mbboDirectRecord.h>

#include <epicsExport.h>

/* directNetAsyn */
#include "devDnAsyn.h"
#include "directNetAsyn.h"
#include "directNetClient.h"


struct wrCache {    /* wrCache is a simple array of all the writable memory */
    struct wrItem {
	unsigned short word;
    } item[WRITEMAXADDR - WRITEMINADDR + 1];
    epicsMutexId mutex;
};

struct dpvtOut {
    struct plcMessage msg;
    char msgData[DN_PLCWORDLEN*2];
    struct dbCommon *precord;
    enum recType {AO, AOF, BO, MBBO, MBBOD} type;
    struct plcAddr plcAddr;
    epicsMutexId mutex;
    struct wrItem *wrItem;
    epicsTimeStamp start;
};


static void ioReport(int detail, struct plcInfo *pPlc) {
    struct wrCache *pcache = pPlc->wrCache;
    
    if (pcache) {
	printf("    WrCache for V%o - V%o starts at %p\n",
		WRITEMINADDR,
		WRITEMAXADDR,
		&(pcache->item[0].word));
    }
    return;
}

static long report(int detail) {
    if (detail > 1) dnAsynReport(detail, ioReport);
    return 0;
}

static void devXoDnCallback(struct plcMessage *pMsg) {
    struct dpvtOut  *dpvt = (struct dpvtOut *) pMsg;
    struct dbCommon *precord=dpvt->precord;
    struct rset     *prset=precord->rset;

    dbScanLock(precord);
    (*prset->process)(precord);
    dbScanUnlock(precord);
}


/* Record init routines */
static long init_output(struct dbCommon *prec, enum recType type, struct link *plink) {
    struct dpvtOut *dpvt;
    struct plcInfo *pPlc;
    struct plcMessage *pMsg;
    struct wrCache *pcache;
    const int numWords = (type == AOF) ? 2 : 1;
    long status;
    
    if (devDnAsynDebug > 0)
	printf ("devXoDnAsyn: init_input invoked for \"%s\"\n", prec->name);
    
    dpvt = (struct dpvtOut *) (calloc(1, sizeof(struct dpvtOut)));
    if (dpvt == NULL) {
	errlogPrintf("devXoDnAsyn: calloc failed for \"%s\"\n", prec->name);
	prec->pact = TRUE;
	return S_rec_outMem;
    }
    prec->dpvt = (void *) dpvt;
    pMsg = &dpvt->msg;
    
    status = dnAsynAddr(prec, &dpvt->plcAddr, plink);
    if (status) {
	prec->pact = TRUE;
	return status;
    }
    pPlc = dpvt->plcAddr.plcInfo;
    
    dpvt->precord = prec;
    dpvt->type    = type;
    
    pMsg->port     = pPlc->port;
    pMsg->proto    = pPlc->proto;
    pMsg->callback = devXoDnCallback;
    
    status = initDnAsynClient(pMsg);
    if (status) {
	prec->pact = TRUE;
	return status;
    }
    
    if ((dpvt->plcAddr.vAddr < WRITEMINADDR+DNREFOFFSET) ||
	(dpvt->plcAddr.vAddr + numWords >= WRITEMAXADDR+DNREFOFFSET)) {
	errlogPrintf("devXoDnAsyn: PLC address write-protected for \"%s\"\n",
		     prec->name);
	prec->pact = TRUE;
	return S_dev_badSignal;
    }
    
    pcache = pPlc->wrCache;
    if (pcache == NULL) {
	/* Not defined?  Create it */
	pcache = (struct wrCache *) calloc(1, sizeof (struct wrCache));
	if (pcache == NULL) {
	    errlogPrintf("devXoDnAsyn: calloc failed for \"%s\"\n", prec->name);
	    prec->pact = TRUE;
	    return S_rec_outMem;
	}
	pcache->mutex = epicsMutexMustCreate();
	pPlc->wrCache = pcache;
    }
    
    dpvt->mutex = pcache->mutex;
    dpvt->wrItem = &pcache->item[dpvt->plcAddr.vAddr - WRITEMINADDR+DNREFOFFSET];
    
    pMsg->cmd   = (pPlc->slaveId << 8) | WRITEVMEM;
    pMsg->len   = numWords * DN_PLCWORDLEN;
    pMsg->addr  = dpvt->plcAddr.vAddr;
    pMsg->pdata = dpvt->msgData;
    
    return 0;
}

static long init_ao(struct aoRecord *prec) {
    long status;
    
    if (devDnAsynDebug > 0)
	printf ("devXoDnAsyn: Init ao invoked\n");
    
    status = init_output((struct dbCommon *) prec, AO, &prec->out);
    if (status)
	prec->pact = TRUE;	/* Error, prevent processing */
    
    return status;
}

static long init_aoflt(struct aoRecord *prec) {
    long status;
    
    if (devDnAsynDebug > 0)
	printf ("devXoDnAsyn: Init ao_float invoked\n");
    
    status = init_output((struct dbCommon *) prec, AOF, &prec->out);
    if (status)
	prec->pact = TRUE;	/* Error, prevent processing */
    
    return status;
}


static long init_bo(struct boRecord *prec) {
    long status;

    if (devDnAsynDebug > 0)
	printf ("devXoDnAsyn: Init bo invoked\n");
 
    status = init_output((struct dbCommon *) prec, BO, &prec->out);
    if (status) {
	prec->pact = TRUE;	/* Error, prevent processing */
	return status;
    }
    
    prec->mask = 1 << ((struct dpvtOut *) prec->dpvt)->plcAddr.bitNum;
    return status;
}


static long init_mbbo(struct mbboRecord *prec) {
    long status;
    
    if (devDnAsynDebug > 0)
       printf ("devXoDnAsyn: Init mbbo invoked\n");
 
    status = init_output((struct dbCommon *) prec, MBBO, &prec->out);
    if (status) {
	prec->pact = TRUE;	/* Error, prevent processing */
	return status;
    }
    
    prec->shft = ((struct dpvtOut *) prec->dpvt)->plcAddr.bitNum;
    prec->mask <<= prec->shft;
    return status;
}


static long init_mbbod(struct mbboDirectRecord *prec) {
    long status;
    
    if (devDnAsynDebug > 0)
       printf ("devXoDnAsyn: Init mbbo invoked\n");
 
    status = init_output((struct dbCommon *) prec, MBBOD, &prec->out);
    if (status) {
	prec->pact = TRUE;	/* Error, prevent processing */
	return status;
    }
    
    prec->shft = ((struct dpvtOut *) prec->dpvt)->plcAddr.bitNum;
    prec->mask <<= prec->shft;
    return status;
}


static void setup_write(struct dbCommon *prec) {
    struct dpvtOut *dpvt = (struct dpvtOut *) prec->dpvt;
    struct wrItem *pitem = dpvt->wrItem;
    unsigned long mask;

    if (devDnAsynDebug >= 35)
        printf ("devXoDnAsyn: setup_write entered for \"%s\"\n", prec->name);

    epicsMutexMustLock(dpvt->mutex);
    switch(dpvt->type) {
	struct aoRecord *ao;
	struct boRecord *bo;
	struct mbboRecord *mbbo;
	struct mbboDirectRecord *mbbod;
	
	case AO:
	    ao = (struct aoRecord *) prec;
	    pitem->word = ao->rval;
	    break;
	
	case AOF:
	    ao = (struct aoRecord *) prec;
	    {
		union {
		    float f;
		    unsigned long l;
		} convert;
		convert.f = ao->oval;
		mask = convert.l;
	    }
	    pitem->word = mask & 0xffff;
	    mask = (mask >> 16) & 0xffff;
	    (pitem + 1)->word = mask;
	    dpvt->msgData[2] = mask & 0xff;
	    dpvt->msgData[3] = (mask >> 8) & 0xff;
	    break;
	
	case BO:
	    bo = (struct boRecord *) prec;
	    mask = bo->mask;
	    pitem->word = (pitem->word & ~mask) | (bo->rval & mask);
	    break;
	
	case MBBO:
	    mbbo = (struct mbboRecord *) prec;
	    mask = mbbo->mask;
	    pitem->word = (pitem->word & ~mask) | (mbbo->rval & mask);
	    break;
	
	case MBBOD:
	    mbbod = (struct mbboDirectRecord *) prec;
	    mask = mbbod->mask;
	    pitem->word = (pitem->word & ~mask) | (mbbod->rval & mask);
	    break;
    }
    dpvt->msgData[0] = pitem->word & 0xff;
    dpvt->msgData[1] = (pitem->word >> 8) & 0xff;
    epicsMutexUnlock(dpvt->mutex);

    if (devDnAsynDebug >= 10) {
	printf("devXoDnAsyn: Send V%o = %d\n", dpvt->msg.addr, *dpvt->msg.pdata);
    }
}


static long write_data(struct dbCommon *prec) {
    struct dpvtOut *dpvt=(struct dpvtOut *)prec->dpvt;
    struct plcMessage *pMsg;
    struct plcInfo *pPlc;

    if (devDnAsynDebug >= 35)
       printf ("devXoDnAsyn: write_data called\n");

    if (!dpvt) return S_dev_NoInit;

    pMsg = &dpvt->msg;
    pPlc = dpvt->plcAddr.plcInfo;
    
    if (!prec->pact) {
	/* Record is idle, start a transaction */
	if (devDnAsynDebug >= 3)
	    epicsTimeGetCurrent(&dpvt->start);
	
	if (devDnAsynDebug >= 10) 
	    printf("devXoDnAsyn: Sending data from \"%s\"\n",
		   prec->name);
	
	/* Send the request */
	setup_write(prec);
	if (dnAsynClientSend(pMsg)) {
	    errlogPrintf("devXoDnAsyn: Asyn Send by \"%s\" failed\n", prec->name);
	    recGblSetSevr(prec, WRITE_ALARM, MAJOR_ALARM);
	    pPlc->nAsynFail++;
	    return -1;
	}
	pPlc->nWrReqs++;
	prec->pact=TRUE;
    } else {
	/* Record busy, a transaction via ASYN has completed */
	if (devDnAsynDebug >= 10) {
	    printf("devXoDnAsyn: Got a reply for \"%s\"\n", 
		    prec->name);
	    printf("devXoDnAsyn: Write V%o = %d returned status %d\n",
		    pMsg->addr, *pMsg->pdata, pMsg->status);
	} else if (pMsg->status && devDnAsynDebug >= 5)
	    printf("devXoDnAsyn: Write reply for \"%s\" has status %d\n",
		   prec->name, pMsg->status);
	
	if (pMsg->status == DN_SUCCESS) {
	    /* OK reply was received */
	    pPlc->alarm = NO_ALARM;
	    pPlc->nSuccess++;

	    if (devDnAsynDebug >= 3) {
		epicsTimeStamp tNow;
		double duration;
		epicsTimeGetCurrent(&tNow);
		duration = epicsTimeDiffInSeconds(&tNow, &dpvt->start);
		printf("devXoDnAsyn: Write for \"%s\" took %f seconds\n", 
			prec->name, duration);
	    }
	} else if (pMsg->status > DN_TIMEOUT) {
	    /* DirectNet I/O problem */
	    errlogPrintf("devXoDnAsyn: DirectNet error %s writing %s to PLC \"%s\" on Asyn port \"%s\"\n",
			 dn_error_strings[pMsg->status], prec->name, pPlc->name, pMsg->port);
	    recGblSetSevr(prec, WRITE_ALARM, INVALID_ALARM); 
	    pPlc->alarm = INVALID_ALARM;
	    pPlc->nDnFail++;
	} else {
	    /* ASYN I/O problem */
	    errlogPrintf("devXoDnAsyn: dnAsyn error %s writing %s to PLC \"%s\" on Asyn port \"%s\"\n",
			 dn_error_strings[pMsg->status], prec->name, pPlc->name, pMsg->port);
	    recGblSetSevr(prec, WRITE_ALARM, MAJOR_ALARM);
	    pPlc->alarm = MAJOR_ALARM;
	    pPlc->nAsynFail++;
	    return -1;
	}
    }

    return 0;
}


/* Device Support Entry Tables */

XXDSET devAoDnAsyn    = { 6,NULL,  NULL,init_ao,    NULL, write_data, NULL };
XXDSET devAoFDnAsyn   = { 6,NULL,  NULL,init_aoflt, NULL, write_data, NULL };
XXDSET devBoDnAsyn    = { 5,report,NULL,init_bo,    NULL, write_data };
XXDSET devMbboDnAsyn  = { 5,NULL,  NULL,init_mbbo,  NULL, write_data };
XXDSET devMbbodDnAsyn = { 5,NULL,  NULL,init_mbbod, NULL, write_data };

epicsExportAddress(dset, devAoDnAsyn);
epicsExportAddress(dset, devAoFDnAsyn);
epicsExportAddress(dset, devBoDnAsyn);
epicsExportAddress(dset, devMbboDnAsyn);
epicsExportAddress(dset, devMbbodDnAsyn);
