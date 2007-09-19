CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = ../../bin

#DEFINES += IRISNET_STATIC
#include(irisnet.pri)

INCLUDEPATH += ../../src/irisnet/corelib
LIBS += -L../../lib -lirisnetcore

SOURCES += main.cpp
