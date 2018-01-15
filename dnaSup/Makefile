TOP=..
include $(TOP)/configure/CONFIG
#=======================================

HTMLS_DIR = .
HTMLS += dnAsyn.html
HTMLS += DL250stat.gif

DBD += devDnAsyn.dbd

# Build love as a library for an IOC:
LIBRARY_IOC += directNetAsyn

# Library Source files
directNetAsyn_SRCS += devDnAsyn.c
directNetAsyn_SRCS += dnAsynInteract.c
directNetAsyn_SRCS += devXoDnAsyn.c
directNetAsyn_SRCS += devXiDnAsyn.c
directNetAsyn_SRCS += directNetClient.c

GIT_VER := $(shell git describe --always --tags --dirty)
dnAsynInteract_CFLAGS = -DGIT_VER=$(GIT_VER)

# Link with the asyn and base libraries
directNetAsyn_LIBS += asyn
directNetAsyn_LIBS += $(EPICS_BASE_IOC_LIBS)

#=======================================
include $(TOP)/configure/RULES
