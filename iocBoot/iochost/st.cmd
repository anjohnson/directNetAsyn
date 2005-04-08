#!../../bin/solaris-sparc/dnaTest
< envPaths

cd ${TOP}
dbLoadDatabase("dbd/dnaTest.dbd")
dnaTest_registerRecordDeviceDriver(pdbbase)

# Init asyn remote IP port
# drvAsynIPPortConfigure 'port name' 'host:port [protocol]' priority 'disable auto-connect' noProcessEos
drvAsynIPPortConfigure serials8n4-1 serials8n4:4001 0 0 0

# Init asyn local serial port
# drvAsynSerialPortConfigure 'port name' 'tty name' priority 'disable auto-connect' noProcessEos
#drvAsynSerialPortConfigure ttya /dev/ttyS0 0 0 0
#asynSetOption serials8n4-1 0 baud 9600
#asynSetOption serials8n4-1 0 bits 8
#asynSetOption serials8n4-1 0 parity odd

# Create PLC:
# createDnAsynPLC 'PLC name' 'directNet slave ID' 'Asyn port name'
createDnAsynPLC test 1 serials8n4-1

# Debugging:
#asynSetTraceMask serials8n4-1 0 19
#asynSetTraceIOMask serials8n4-1 0 4
#var devDnAsynDebug

dbLoadRecords db/DL250stat.db "name=dnaTest,plc=test"
dbLoadRecords db/dnPlcTest.db "name=dnaTest,plc=test"

cd ${TOP}/iocBoot/${IOC}
iocInit()
