IRIS_BASE = ../..

CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = ../../bin

INCLUDEPATH += ../../include

# bundle just irisnet
#include(../../src/irisnet/noncore/noncore.pri)

# link against just irisnet
#LIBS += -L$$IRIS_BASE/lib -lirisnet

# bundle all of iris
include(../../iris.pri)

SOURCES += main.cpp
