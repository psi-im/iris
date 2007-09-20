IRIS_BASE = ../..
include(../../build.pri)

CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = ../../bin

INCLUDEPATH += ../../include ../../include/iris

iris_bundle:{
	include(../../src/irisnet/noncore/noncore.pri)
}
else {
	LIBS += -L$$IRIS_BASE/lib -lirisnet
}

SOURCES += main.cpp
