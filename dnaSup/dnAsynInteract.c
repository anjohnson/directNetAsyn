/******************************************************************************

Project:
    DirectNet ASYN

File:
    dnAsynInteract.c

Description:
    Interact with a directNet PLC over ASYN

Author:
    Andrew Johnson
Version:
    $Id$

******************************************************************************/

/* OS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* libCom */
#include <epicsEvent.h>
#include <epicsExport.h>
#include <epicsReadline.h>
#include <epicsStdio.h>
#include <iocsh.h>

#define epicsExportSharedSymbols

/* directNetAsyn */
#include "devDnAsyn.h"
#include "directNetAsyn.h"
#include "directNetClient.h"


/* Some local magic numbers */

#define DNI_MAXARGS	5

#define STRINGIFY(x) #x
#define VALUE(x) STRINGIFY(x)
#define GIT_VERSION VALUE(GIT_VER)

/* Declarations and definitions */

struct plcInteract {
    struct plcMessage msg;	/* Must be first member */
    struct plcInfo *pPlc;
    epicsEventId replied;
    char rdData[DN_RDDATA_MAX];
    struct plcInteract *pNext;
};

static struct plcInteract *plcFirst = NULL;


struct plcIoType {
    const char *letters;	/* How do we recognize an addr? */
    int addrWidth;		/* How many digits in address output */
    char addrBase;		/* Number base for address output */
    enum {BIT, WORD} aType;	/* Whether number refers to bits or words */
    int readCmd;		/* DirectNet read command to use */
    int wordLen;		/* How many bytes make up a word */
    int lineLen;		/* How many data values fit on a 'd' line */
    int dnOffset; 		/* Add to PLC addr to get DN addr */
    int maxAddr;		/* Largest legal PLC addr */
    const char *desc;
};

static const struct plcIoType plcIoData[] = {
    {"V",   5,  8, WORD, READVMEM, 2,  8, 00001, 041237, "V-memory"},
    {"CTA", 3,  8, WORD, READVMEM, 2,  8, 01001,    127, "Counter value"},
    {"CT",  3,  8, BIT,  READOUTS, 1,  2, 0x001,    127, "Counter status"},
    {"C",   4,  8, BIT,  READOUTS, 1,  2, 0x181,   1023, "Control relay"},
    {"F",   5,  8, WORD, READVMEM, 4,  2, 00001, 041237, "Floating point"},
    {"SP",  3,  8, BIT,  READINPS, 1,  2, 0x181,    511, "Special relay"},
    {"S",   4,  8, BIT,  READOUTS, 1,  2, 0x281,   1023, "Stage status"},
    {"TA",  3,  8, WORD, READVMEM, 2,  8, 00001,    255, "Timer value"},
    {"T",   3,  8, BIT,  READOUTS, 1,  2, 0x301,    255, "Timer status"},
    {"X",   3,  8, BIT,  READINPS, 1,  2, 0x101,    511, "Digital input"},
    {"Y",   3,  8, BIT,  READOUTS, 1,  2, 0x101,    511, "Digital output"},
 
    {"L",   4, 16, WORD, READPROG, 3,  5, 0x000, 0x1dff, "Ladder program"},
    {"Z",   4, 16, WORD, READSPAD, 1, 16, 0x000, 0x5101, "Scratchpad"},
};


/* Routines */

static void dniCallback(struct plcMessage *pMsg) {
    struct plcInteract *pInt = (struct plcInteract *) pMsg;
    
    epicsEventSignal(pInt->replied);
}


static void dniConnect(struct plcInteract **ppInt, const char *pname) {
    struct plcInteract *pInt;
    struct plcInfo *pPlc;
    if (pname == NULL) {
	puts("The following PLCs are registered with dnAsyn:");
	dnAsynReport(0, NULL);
	return;
    }
    
    /* Search our records first */
    for (pInt = plcFirst; pInt; pInt = pInt->pNext) {
	if (strcmp(pname, pInt->pPlc->name) == 0)
	    break;
    }
    
    if (pInt == NULL) {
	pPlc = dnAsynPlc(pname);
	if (pPlc == NULL) {
	    printf("No PLC named \"%s\" is registered with dnAsyn\n", pname);
	    dnAsynReport(0, NULL);
	    return;
	}
	
	pInt = calloc(1, sizeof(struct plcInteract));
	if (pInt == NULL) {
	    perror("DNI");
	    return;
	}
	
	pInt->msg.port     = pPlc->port;
	pInt->msg.pdata    = pInt->rdData;
	pInt->msg.callback = dniCallback;
	
	if (initDnAsynClient(&pInt->msg)) {
	    puts("DNI: initDnAsynClient failed - can't bind to ASYN port?");
	    free(pInt);
	    return;
	}
	
	pInt->replied = epicsEventCreate(epicsEventEmpty);
	if (pInt->replied == NULL) {
	    perror("DNI");
	    return;
	}
	
	pInt->pPlc = pPlc;
	pInt->pNext = plcFirst;
	plcFirst = pInt;
    }
    *ppInt = pInt;
    pPlc = pInt->pPlc;
    printf("Connected to PLC \"%s\" on ASYN port \"%s\", DirectNet ID %u\n",
    	    pPlc->name, pPlc->port, pPlc->slaveId);
}


static void dniHelp(const char *pcmd) {
    if (pcmd == NULL) pcmd = "";
    switch (tolower((int) *pcmd)) {
    int i;
    case 0:
	puts("    The DNI commands available are:\n"
	     "\t? [cmd]\t\t- Display help [on cmd]\n"
	     "\tc <plcName>\t- Connect to PLC <plcName>\n"
	     "\td <addr> [n]\t- Display PLC data at <addr>\n"
	     "\tm <addr>\t- Modify PLC data at <addr>\n"
	     "\tq\t\t- Quit DNI\n"
	     "\tr\t\t- Print database I/O Report for all PLCs\n"
	     "\ts\t\t- Get PLC communications status\n"
	     "\tv\t\t- Print directNetAsyn Version"
	     "    <addr> is a PLC data type followed by the address, eg V02000\n"
	     "    [n] is an optional element count\n"
	     "    Type '? d' for a list of address types supported");
	break;
    case 'd':
	puts("    The 'd' can command dump the contents of various PLC locations.\n"
	     "    Valid DL250 PLC address types and ranges are:");
	for (i=0; i<sizeof(plcIoData) / sizeof(plcIoData[0]); i++) {
	    const struct plcIoType *pio = &plcIoData[i];
	    char addrFormat = (pio->addrBase == 8) ? 'o' : 'X';
	    char msgbuf[80];
	    
	    sprintf(msgbuf, "\t%-*s %s%%0%d%c - %s%%0%d%c\n",
		    (int)(23 - pio->addrWidth - strlen(pio->letters)),
		    pio->desc, pio->letters, pio->addrWidth, addrFormat,
		    pio->letters, pio->addrWidth, addrFormat);
	    printf(msgbuf, 0, pio->maxAddr);
	}
	puts("    L and Z addresses are in hex, other addresses in octal.\n"
	     "    Word data values are output in hex, Bit data in binary.");
	break;
    case 'm':
	puts("    The 'm' command modifies VMEM locations so addr must be octal.\n"
	     "    Data values from the PLC are displayed in hex.\n"
	     "    At the '>' prompt, a number entry (with leading 0 or 0x for octal\n"
	     "    or hex values respectively) will be written back to the location,\n"
	     "    or the following subcommands can be used:\n"
	     "\t^   - Set direction backwards (decreasing addresses)\n"
	     "\t=   - Set direction stationary (same address)\n"
	     "\tv   - Set direction forwards (increasing addresses)\n"
	     "\t-   - Move to preceeding address (direction setting is ignored)\n"
	     "\t+   - Move to following address (direction setting is ignored)\n"
	     "\t?   - Prints this help message\n"
	     "\t.   - Quit (unrecognized commands also quit)");
	break;
    case 'u':
	if (strcmp(pcmd, "unprotect") == 0) {
	    puts("    The 'unprotect' command disables the write-protection limits\n"
		 "    for the 'm' command, allowing engineers to subsequently modify\n"
		 "    any VMEM location in the PLC.  USE THIS WITH CARE!\n"
		 "    Protection is reinstated by the 'c' or 'u' commands");
	    return;
	}
	/* Fall through */
    default:
	puts("    No extended help is available for that command");
	break;
    }
}


static int dniSend(struct plcInteract *pInt) {
    int status = dnAsynClientSend(&pInt->msg);
    if (status) {
	printf("\nDNI: ASYN Send returned %d\n", status);
	return status;
    }

    epicsEventMustWait(pInt->replied);	/* wait for callback */

    if (pInt->msg.status) {
	printf("\nDNI: Reply status = %s\n",
		dn_error_strings[pInt->msg.status]);
    }
    return pInt->msg.status;
}


static void dniDump(struct plcInteract *pInt, const char *paddr, 
		    const char *plen) {
    struct plcMessage *pMsg = &pInt->msg;
    static const struct plcIoType *pio = plcIoData;
    static int addr = 0;
    static int defLen = 64;
    char addrFormat[20];
    char addrConv;
    int len = defLen;
    int i;
    
    if (paddr) {
	if (isdigit((int)paddr[0])) {
	    /* Assume it's a VMEM address */
	    pio = plcIoData;
	} else {
	    const int numIo = sizeof(plcIoData) / sizeof(plcIoData[0]);
	    for (i=0; i<numIo; i++) {
		if (strncmp(paddr, plcIoData[i].letters, strlen(plcIoData[i].letters)) == 0) {
		    pio = &plcIoData[i];
		    paddr += strlen(plcIoData[i].letters);
		    break;
		}
	    }
	    if (i == numIo) {
		puts("DNI: PLC address type not recognized.");
		return;
	    }
	}
	addr = strtoul(paddr, NULL, pio->addrBase);
    }
    
    if (plen) {
	defLen = len = strtoul(plen, NULL, 0);
    } else {
	if (addr + len - 1 > pio->maxAddr) {
	    len = pio->maxAddr - addr + 1;
	    if (len == 0) {
		puts("DNI: Reached end of valid address range");
		return;
	    }
	}
    }
    
    if (addr + len - 1 > pio->maxAddr) {
	puts("DNI: Argument(s) out of range");
	return;
    }
    
    if (pio->aType == BIT) {
	len = (len + 7) / 8;
    } else {
	len *= pio->wordLen;
    }
    /* len is now a byte count */
    
    addrConv = (pio->addrBase == 8) ? 'o' : 'X';
    sprintf(addrFormat, "\n%s%%0%d%c:", pio->letters, pio->addrWidth, addrConv);
    
    pMsg->cmd  = (pInt->pPlc->slaveId << 8) | pio->readCmd;
    
    while (len > 0) {
	const int maxRead = DN_RDDATA_MAX - (DN_RDDATA_MAX % pio->wordLen);
	
	if (pio->aType == BIT) {
	    pMsg->addr = (addr / 8) + pio->dnOffset;
	} else {
	    pMsg->addr = addr + pio->dnOffset;
	}
	pMsg->len  = (len < maxRead) ? len : maxRead;
	
	if (dniSend(pInt)) return;
	
	for (i=0; i < (pMsg->len / pio->wordLen); i++) {
	    if (i % pio->lineLen == 0) {
		printf(addrFormat, addr);
	    }
	    switch (pio->wordLen) {
	    union {
                epicsUInt32 u32;
                float f32;
            } datum;
	    case 4:
		datum.u32 = ((0xff & pMsg->pdata[i*4+3]) << 24) +
			    ((0xff & pMsg->pdata[i*4+2]) << 16) +
			    ((0xff & pMsg->pdata[i*4+1]) << 8) +
			     (0xff & pMsg->pdata[i*4  ]);
		printf(" %8.8X=%-8.5g", datum.u32, datum.f32);
		addr += 2;
		break;
		
	    case 3:
		datum.u32 = ((0xff & pMsg->pdata[i*3+2]) << 16) +
			    ((0xff & pMsg->pdata[i*3+1]) << 8) +
			     (0xff & pMsg->pdata[i*3  ]);
		printf(" %6.6X", datum.u32);
		addr += 1;
		break;
		
	    case 2:
		datum.u32 = ((0xff & pMsg->pdata[i*2+1]) << 8) +
			     (0xff & pMsg->pdata[i*2  ]);
		printf(" %4.4X", datum.u32);
		addr += 1;
		break;
		
	    case 1:
		datum.u32 = (0xff & pMsg->pdata[i]);
		if (pio->aType == BIT) {
		    int j;
		    for (j=0; j<8; j++) {
			printf(" %c", (datum.u32 & 1) ? '1' : '0');
			datum.u32 >>= 1;
		    }
		    addr += 8;
		} else {
		    printf(" %2.2X", datum.u32);
		    addr += 1;
		}
		break;
		
	    default:
		puts("DNI: Internal error, unknown wordLen");
		return;
	    }
	}
	
	len  -= pMsg->len;
    }
    
    putchar('\n');
}

static void dniModify(struct plcInteract *pInt, const char *paddr, int protect) {
    struct plcMessage *pMsg = &pInt->msg;
    const struct plcIoType *pio = plcIoData;
    void *readlineContext;
    int addr = 0;
    int delta = 1;
    int quit = FALSE;
    
    if (!paddr) {
	puts("DNI: PLC address not given");
	return;
    }
    
    if (!isdigit((int)paddr[0])) {
	if (paddr[0] == 'V') {
	    paddr++;
	} else {
	    puts("DNI: Only VMEM addresses can be modified");
	    return;
	}
    }
    addr = strtoul(paddr, NULL, pio->addrBase);
    
    if (addr > pio->maxAddr) {
	puts("DNI: Address out of valid range");
	return;
    }
    
    if (protect && ((addr < WRITEMINADDR) || (addr > WRITEMAXADDR))) {
	puts("DNI: Write protected address");
	return;
    }
    
    readlineContext = epicsReadlineBegin(NULL);
    
    while (!quit) {
	int datum;
	char prompt[40];
	char *input;
	char *parse;
	
	pMsg->cmd  = (pInt->pPlc->slaveId << 8) | pio->readCmd;
	pMsg->addr = addr + pio->dnOffset;
	pMsg->len  = pio->wordLen;
	
	if (dniSend(pInt)) return;
	
	datum = ((0xff & pMsg->pdata[1]) << 8) +
		 (0xff & pMsg->pdata[0]);
	epicsSnprintf(prompt, sizeof(prompt), "%s%0*o: 0x%4.4X > ",
		      pio->letters, pio->addrWidth, addr,
		(unsigned int) datum);
	input = epicsReadline(prompt, readlineContext);
	
	datum = strtol(input, &parse, 0);
	if (parse != input) {
	    pMsg->cmd |= WRITECMD;
	    pMsg->pdata[0] = datum & 0xff;
	    pMsg->pdata[1] = (datum >> 8) & 0xff;
	    
	    if (dniSend(pInt)) return;
	}
	
	while (isspace((int)*parse)) parse++;
	
	switch (*parse) {
	case 0:
	    break;
	case '-':
	    addr = addr - 1 - delta;
	    break;
	case '+':
	    addr = addr + 1 - delta;
	    break;
	case '=':
	    delta = 0;
	    break;
	case '^':
	    delta = -1;
	    break;
	case 'v':
	case 'V':
	    delta = 1;
	    break;
	case '?':
	    dniHelp("m");
	    addr -= delta;
	    break;
	default:
	    quit = TRUE;
	    break;
	}
	
	addr += delta;
	if (protect) {
	    if ((addr < WRITEMINADDR) || (addr > WRITEMAXADDR)) {
		puts("End of unprotected area");
		quit = TRUE;
	    }
	} else {
	    if ((addr < 0) || (addr > pio->maxAddr)) {
		puts("End of address space");
		quit = TRUE;
	    }
	}
    }
    epicsReadlineEnd(readlineContext);
}


static void dniStatus(struct plcInteract *pInt) {
    struct plcMessage *pMsg = &pInt->msg;
    pMsg->cmd  = (pInt->pPlc->slaveId << 8) | READSTAT;
    pMsg->addr = 0;
    pMsg->len  = 10;
    
    if (dniSend(pInt)) return;
    
    printf(" Last error code:  0x%2.2X\n", 0xff & pMsg->pdata[0]);
    printf(" Prev error code:  0x%2.2X\n", 0xff & pMsg->pdata[1]);

    printf("Successful comms: %5d\n",
	   ((0xff & pMsg->pdata[3]) << 8) | (0xff & pMsg->pdata[2]));
    printf(" Erroneous comms: %5d\n",
	   ((0xff & pMsg->pdata[5]) << 8) | (0xff & pMsg->pdata[4]));
    printf("  Header retries: %5d\n",
	   ((0xff & pMsg->pdata[7]) << 8) | (0xff & pMsg->pdata[6]));
    printf("    Data retries: %5d\n",
	   ((0xff & pMsg->pdata[9]) << 8) | (0xff & pMsg->pdata[8]));
}


int DNI(const char *pname) {
    static void *readlineContext = NULL;
    struct plcInteract *pInt = NULL;
    struct plcInfo *pPlc;
    char prompt[32];
    int protect = TRUE;
    char cmd = 0;
    
    puts("\nDirectNet Interaction program\n");
    
    if (!readlineContext) {
	readlineContext = epicsReadlineBegin(NULL);
	if (!readlineContext) {
	    puts("Can't allocate command-line object.\n");
	    return -1;
	}
    }
    
    if (pname == NULL || *pname == '\0') {
	puts("Usage:\n\tDNI \"plcName\"\n");
	/* dniConnect() below outputs the list of known PLCs and exits */
    }
    
    dniConnect(&pInt, pname);
    if (pInt == NULL) {
	return -1;
    }
    pPlc = pInt->pPlc;
    epicsSnprintf(prompt, sizeof(prompt), "DNI:%s> ", pPlc->name);
    
    puts("Enter a command, ? gives help");
    
    while (cmd != 'q') {
	static const char *separators = " \t\n\r";
	char *input;
	char *argv[DNI_MAXARGS + 1];
	char *pLast = NULL;
	int i, argc = 0;
	
	input = epicsReadline(prompt, readlineContext);
	if (!input) break;
	
	argv[0] = strtok_r(input, separators, &pLast);
	if (argv[0] == NULL) continue;
	while (argv[argc] && argc < DNI_MAXARGS) {
	    argv[++argc] = strtok_r(NULL, separators, &pLast);
	}
	for (i=argc; i <= DNI_MAXARGS; i++) {
	    argv[i] = NULL;
	}
	
	cmd = tolower((int) *argv[0]);
	switch (cmd) {
	case '?':
	    dniHelp(argv[1]);
	    break;
	    
	case 'c':
	    dniConnect(&pInt, argv[1]);
	    pPlc = pInt->pPlc;
	    epicsSnprintf(prompt, sizeof(prompt), "DNI:%s> ", pPlc->name);
	    protect = TRUE;
	    break;
	    
	case 'd':
	    dniDump(pInt, argv[1], argv[2]);
	    break;
	    
	case 'm':
	    dniModify(pInt, argv[1], protect);
	    break;
	    
	case 'p':
	    /* Program */
	    puts("Command not yet implemented");
	    break;
	    
	case 'q':
	    /* QUIT - fall out of while loop*/
	    break;
	    
	case 'r':
	    dnAsynReport(1, NULL);
	    break;
	    
	case 's':
	    dniStatus(pInt);
	    break;
	    
	case 'u':
	    /* A hidden command, allows writes to any VMEM addr */
	    protect = strcmp(argv[0], "unprotect");
	    printf("VMEM Write protection is %s\n", protect ? "ON" : "OFF");
	    break;
	    
	case 'v':
	    printf("directNetAsyn Git Version: %s\n",
		   GIT_VERSION);
	    break;
	    
	default:
	    puts("Command not recognized");
	    break;
	}
    }
    
    return 0;
}


/* Command registry data:
 * 	DNI(const char *pname)
 */
static const iocshArg cmd0Arg0 = { "PLC name",iocshArgString};
static const iocshArg * const cmd0Args[] = {&cmd0Arg0};
static const iocshFuncDef cmd0FuncDef =
    {"DNI", 1, cmd0Args};
static void cmd0CallFunc(const iocshArgBuf *args)
{
    DNI(args[0].sval);
}

/* Registrar routine */
void dniAsynRegistrar(void) {
    iocshRegister(&cmd0FuncDef, cmd0CallFunc);
}
epicsExportRegistrar(dniAsynRegistrar);
