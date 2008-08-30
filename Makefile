# Makefile for directNetAsyn

TOP = .
include $(TOP)/configure/CONFIG

DIRS := configure

DIRS += dnaSup
dnaSup_DEPEND_DIRS = configure

DIRS += DL250plc

DIRS += test
test_DEPEND_DIRS = dnaSup

DIRS += iocBoot
iocBoot_DEPEND_DIRS = configure

include $(TOP)/configure/RULES_TOP
