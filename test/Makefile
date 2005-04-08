TOP=..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

ADLS += dnPlcTest.adl
DB += dnPlcTest.db

DBD += dnaTest.dbd

dnaTest_DBD += base.dbd
dnaTest_DBD += asyn.dbd
dnaTest_DBD += drvAsynSerialPort.dbd
dnaTest_DBD += drvAsynIPPort.dbd
dnaTest_DBD += devDnAsyn.dbd

#=============================

PROD_IOC_DEFAULT += dnaTest

dnaTest_SRCS += dnaTest_registerRecordDeviceDriver.cpp
dnaTest_SRCS += dnaMain.c

dnaTest_LIBS += directNetAsyn
dnaTest_LIBS += asyn
dnaTest_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

