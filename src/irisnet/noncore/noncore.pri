IRIS_BASE = $$PWD/../../..

QT *= network

irisnetcore_bundle:{
	include(../corelib/corelib.pri)
}
else {
	LIBS += -L$$IRIS_BASE/lib -lirisnetcore
}

INCLUDEPATH += $$PWD/../corelib

HEADERS += \
	$$PWD/processquit.h \
	$$PWD/stunutil.h \
	$$PWD/stunmessage.h \
	$$PWD/stuntypes.h \
	$$PWD/stuntransaction.h \
	$$PWD/stunbinding.h \
	$$PWD/stunallocate.h \
	$$PWD/icelocaltransport.h \
	$$PWD/ice176.h

SOURCES += \
	$$PWD/processquit.cpp \
	$$PWD/stunutil.cpp \
	$$PWD/stunmessage.cpp \
	$$PWD/stuntypes.cpp \
	$$PWD/stuntransaction.cpp \
	$$PWD/stunbinding.cpp \
	$$PWD/stunallocate.cpp \
	$$PWD/icelocaltransport.cpp \
	$$PWD/ice176.cpp

include(legacy/legacy.pri)
