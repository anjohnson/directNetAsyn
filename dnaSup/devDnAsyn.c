/******************************************************************************

Project:
    DirectNet ASYN

File:
    devDnAsyn.c

Description:
    Common device support routines for directNet over ASYN

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

/* OS */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* libCom */
#include <alarm.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <iocsh.h>

/* IOC */
#include <dbCommon.h>
#include <dbScan.h>
#include <devSup.h>
#include <drvSup.h>
#include <link.h>
#include <recGbl.h>

#include <epicsExport.h>

/* directNetAsyn */
#include "directNetClient.h"
#include "devDnAsyn.h"


/* Global variables */

int devDnAsynDebug = 0;
epicsExportAddress(int,devDnAsynDebug);

/* Module variables */

static struct plcInfo *dnAsyn_plcs;


/* Find a named PLC */

struct plcInfo * dnAsynPlc(const char* pname) {
    struct plcInfo *pPlc = dnAsyn_plcs;
    
    while (pPlc) {
	if (strcmp(pname, pPlc->name) == 0)
	    return pPlc;
	pPlc = pPlc->pNext;
    }
    return NULL;
}


/* Parse an instio link field */

int dnAsynAddr(struct dbCommon *prec, struct plcAddr *paddr, struct link *plink) {
    struct instio *pinstio;
    struct plcInfo *pPlc;
    unsigned int addr;
    char *parse;
    int addrType = -1;
    int i;
    
    static const struct {
	const char *letters;	/* How do we introduce it? */
	enum {BIT, WORD} aType;	/* Whether addr refers to bits or words */
	unsigned int offset;	/* Starting V-memory address (NB octal) */
	unsigned int maxAddr;	/* largest legal bit/address */
    } aTypes[] = {	/* NB: Relative order of entries is important */
	{"B",   WORD, 000000, 041237},
	{"CTA", WORD, 001000,   0377},
	{"CT",  BIT,  041140,   0377},
	{"C",   BIT,  040600,  03777},
	{"GX",  BIT,  040000,  03777},
	{"GY",  BIT,  040200,  03777},
	{"SP",  BIT,  041200,   0777},
	{"S",   BIT,  041000,  01777},
	{"TA",  WORD, 000000,   0377},
	{"T",   BIT,  041100,   0377},
	{"V",   WORD, 000000, 041237},
	{"X",   BIT,  040400,  01777},
	{"Y",   BIT,  040500,  01777},
    };
    
    /* link address must be INST_IO */
    if (plink->type != INST_IO) {
	recGblRecordError(S_dev_badBus, (void *) prec,
	    "devDnAsyn (init_record) Illegal Bus Type");
	return S_dev_badBus;
    }
    
    /* check the string starts with a recognized PLC name */
    pinstio = &plink->value.instio;
    parse = pinstio->string;
    for (pPlc = dnAsyn_plcs; pPlc; pPlc = pPlc->pNext) {
	i = strlen(pPlc->name);
	if ((strncmp(parse, pPlc->name, i) == 0) &&
	    parse[i] == ' ') {
	    parse += i + 1;
	    break;
	}
    }
    if ((pPlc == 0) || (parse == pinstio->string)) {
	recGblRecordError(S_dev_badCard, (void *) prec,
	    "devDnAsyn (init_record) named PLC not found");
	return S_dev_badCard;
    }
    paddr->plcInfo = pPlc;
    
    /* parse the PLC address type */
    for (i=0; i < sizeof(aTypes) / sizeof(aTypes[0]); i++) {
	if (strncmp(parse, aTypes[i].letters, strlen(aTypes[i].letters)) == 0) {
	    addrType = i;
	    break;
	}
    }
    if (addrType < 0) {
	recGblRecordError(S_dev_badSignal, (void *) prec,
	    "devDnAsyn (init_record) PLC address type not recognized");
	return S_dev_badSignal;
    }
    parse += strlen(aTypes[addrType].letters);
    
    /* digits giving the PLC address */
    addr = strtoul(parse, &parse, 8);
    if (addr > aTypes[addrType].maxAddr) {
	recGblRecordError(S_dev_badSignal, (void *) prec,
		"devDnAsyn (init_record) PLC address too large");
	return S_dev_badSignal;
    }
    
    paddr->vAddr = aTypes[addrType].offset + DNREFOFFSET;
    if (aTypes[addrType].aType == BIT) {
	paddr->vAddr += (addr / PLCWORDBITS);
	paddr->bitNum = addr % PLCWORDBITS;
	/* This code is wrong if offset is not a multiple of 16 */
    } else {
	paddr->vAddr += addr;
	
	if (*parse == '.') {
	    parse++;
	    paddr->bitNum = strtoul(parse, &parse, 8);
	}
	else
	    paddr->bitNum = 0;
    }
    
    return 0;
}


/* Create plc information entry */

static int addPLC(const char* pname, int slaveId,
    const char* port, const struct plcProto *proto) {
    struct plcInfo *pPlc = dnAsyn_plcs;
    
    if ((slaveId <= 0) || (slaveId > MAXDNSLAVEID)) {
	printf("createDnAsynPLC: Slave ID out of range 1 .. %d\n", MAXDNSLAVEID);
	return -1;
    }
    
    /* Check for duplicates */
    while (pPlc) {
	if (strcmp(pname, pPlc->name) == 0) {
	    printf("createDnAsynPLC: Duplicate name to PLC on port \"%s\" ID %u\n",
		   pPlc->port, pPlc->slaveId);
	    return -1;
	}
	if ((slaveId == pPlc->slaveId) &&
	    (strcmp(port, pPlc->port) == 0)) {
	    printf("createDnAsynPLC: Duplicate address to PLC \"%s\"\n",
		   pPlc->name);
	    return -1;
	}
	pPlc = pPlc->pNext;
    }
    
    /* Create and populate a new info structure for this plc */
    pPlc = (struct plcInfo *) (calloc(1, sizeof(struct plcInfo)));
    if (pPlc == NULL) {
	perror("createDnAsynPLC");
	return -1;
    }

    pPlc->name    = epicsStrDup(pname);
    pPlc->port    = epicsStrDup(port);
    pPlc->proto   = proto;
    pPlc->slaveId = slaveId;

    /* Add it to the list */
    pPlc->pNext = dnAsyn_plcs;
    dnAsyn_plcs = pPlc;
    
    return 0;
}

int createDnAsynPLC(const char* pname, int slaveId,
    const char* port) {
    return addPLC(pname, slaveId, port, &dnpProto);
}

int createDnAsynSimulatedPLC(const char* pname, int slaveId,
    const char* port) {
    return addPLC(pname, slaveId, port, &simProto);
}



/* Report functions */

void dnAsynReport(int detail, dnPlcReportFn ioReport) {
    struct plcInfo *pPlc = dnAsyn_plcs;
    
    while (pPlc) {
	printf("PLC \"%s\" via ASYN port \"%s\" with DirectNet ID %u\n",
		pPlc->name, pPlc->port, pPlc->slaveId);
	switch (detail) {
	    case 0:
		break;
		
	    case 1:
		printf("    alarm = %hu, nRdReqs = %lu, nWrReqs = %lu\n",
			pPlc->alarm, pPlc->nRdReqs, pPlc->nWrReqs);
		printf("    nSuccess = %lu, nDnFail = %lu, nAsynFail = %lu\n",
			pPlc->nSuccess, pPlc->nDnFail, pPlc->nAsynFail);
		break;
		
	    default:
		if (ioReport != NULL)
		    (*ioReport)(detail, pPlc);
	}
	
	pPlc = pPlc->pNext;
    }
}

static long report(int detail) {
    if (detail <= 1)
	dnAsynReport(detail, NULL);
    return 0;
}

/* Create a drvet for the report function */
drvet drvDnAsyn = {2, report, NULL};
epicsExportAddress(drvet, drvDnAsyn);


/* Command registry data:
 * 	createDnAsynPLC(const char* pname, int slaveId, const char* port)
 * 	dnAsynReport(int detail)
 */
static const iocshArg cmd0Arg0 = { "PLC name",iocshArgString};
static const iocshArg cmd0Arg1 = { "directNet slave ID",iocshArgInt};
static const iocshArg cmd0Arg2 = { "Asyn port name",iocshArgString};
static const iocshArg * const cmd0Args[] =
    {&cmd0Arg0,&cmd0Arg1,&cmd0Arg2};
static const iocshFuncDef cmd0FuncDef =
    {"createDnAsynPLC", 3, cmd0Args};
static void cmd0CallFunc(const iocshArgBuf *args)
{
    createDnAsynPLC(args[0].sval, args[1].ival, args[2].sval);
}

static const iocshArg cmd1Arg0 = { "detail",iocshArgInt};
static const iocshArg * const cmd1Args[] = {&cmd1Arg0};
static const iocshFuncDef cmd1FuncDef =
    {"dnAsynReport", 1, cmd1Args};
static void cmd1CallFunc(const iocshArgBuf *args)
{
    dnAsynReport(args[0].ival, NULL);
}

static const iocshFuncDef cmd2FuncDef =
    {"createDnAsynSimulatedPLC", 3, cmd0Args};
static void cmd2CallFunc(const iocshArgBuf *args)
{
    createDnAsynSimulatedPLC(args[0].sval, args[1].ival, args[2].sval);
}


/* Registrar routine */
void devDnAsynRegistrar(void) {
    iocshRegister(&cmd0FuncDef, cmd0CallFunc);
    iocshRegister(&cmd1FuncDef, cmd1CallFunc);
    iocshRegister(&cmd2FuncDef, cmd2CallFunc);
}
epicsExportRegistrar(devDnAsynRegistrar);
