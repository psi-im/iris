IRIS_BASE = $$PWD/../../..

include(../../../build.pri)

DEFINES += IRISNET_STATIC

irisnetcore_bundle:{
	include(../corelib/corelib.pri)
}
else {
	LIBS += -L$$IRIS_BASE/lib -lirisnetcore
}

INCLUDEPATH += $$PWD/../corelib

HEADERS += \
	$$PWD/processquit.h

SOURCES += \
	$$PWD/processquit.cpp
