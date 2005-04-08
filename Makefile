# Makefile for Asyn dg535 support
#
# Created by mrk on Wed Jun 16 13:03:13 2004
# Based on the Asyn devGpib template

TOP = .
include $(TOP)/configure/CONFIG

DIRS := configure
DIRS += dnaSup
DIRS += DL250plc
DIRS += test
DIRS += iocBoot

include $(TOP)/configure/RULES_TOP
