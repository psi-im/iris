IRIS_BASE = $$PWD/../../..

DEFINES += IRISNET_STATIC

#LIBS += -L$$IRIS_BASE/lib -lirisnetcore
include(../corelib/corelib.pri)
INCLUDEPATH += $$PWD/../corelib

HEADERS += \
	$$PWD/processquit.h

SOURCES += \
	$$PWD/processquit.cpp
