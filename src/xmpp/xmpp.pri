IRIS_BASE = $PWD/../..
include(../../build.pri)

INCLUDEPATH += $$PWD/../irisnet/corelib $$PWD/../irisnet/noncore
iris_bundle:{
	include(../irisnet/noncore/noncore.pri)
}
else {
	LIBS += -L$$IRIS_BASE/lib -lirisnet
}

HEADERS += \
	$$PWD/jid.h

SOURCES += \
	$$PWD/jid.cpp
