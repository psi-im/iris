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
	$$PWD/stunmessage.h \
	$$PWD/stuntransaction.h \
	$$PWD/ice176.h

SOURCES += \
	$$PWD/processquit.cpp \
	$$PWD/stunmessage.cpp \
	$$PWD/stuntransaction.cpp \
	$$PWD/ice176.cpp

include(legacy/legacy.pri)
