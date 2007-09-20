IRIS_BASE = ../..

CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = ../../bin

INCLUDEPATH += ../../include
#include(../../src/irisnet/noncore/noncore.pri)
#LIBS += -L$$IRIS_BASE/lib -lirisnet
include(../../iris.pri)

SOURCES += main.cpp
