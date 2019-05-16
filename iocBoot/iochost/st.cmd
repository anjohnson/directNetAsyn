#!../../bin/linux-x86_64/dnaTest
< envPaths

cd ${TOP}
dbLoadDatabase("dbd/dnaTest.dbd")
dnaTest_registerRecordDeviceDriver(pdbbase)

# Init asyn remote IP port
# drvAsynIPPortConfigure 'port name' 'host:port [protocol]' priority 'disable auto-connect' noProcessEos
#drvAsynIPPortConfigure Moxa01-1 moxa01:4001 0 0 0
drvAsynIPPortConfigure SimPort localhost:9999 0 0 0

# Init asyn local serial port
# drvAsynSerialPortConfigure 'port name' 'tty name' priority 'disable auto-connect' noProcessEos
#drvAsynSerialPortConfigure ttya /dev/ttyS0 0 0 0
#asynSetOption Moxa01-1 0 baud 9600
#asynSetOption Moxa01-1 0 bits 8
#asynSetOption Moxa01-1 0 parity odd

# Create PLC or Simulator:
# createDnAsynPLC 'PLC name' 'directNet slave ID' 'Asyn port name'
#createDnAsynPLC test 1 Moxa01-1

# createDnAsynSimulatedPLC 'PLC name' 'slave ID' 'Asyn port name'
createDnAsynSimulatedPLC test 1 SimPort

# Debugging:
#var devDnAsynDebug
#asynSetTraceMask Moxa01-1 0 19
#asynSetTraceIOMask Moxa01-1 0 4
#asynSetTraceMask SimPort 0 19
#asynSetTraceIOMask SimPort 0 4

#dbLoadRecords db/DL250stat.db "name=dnaTest,plc=test"
dbLoadRecords db/dnPlcTest.db "name=dnaTest,plc=test"

cd ${TOP}/iocBoot/${IOC}
iocInit()
