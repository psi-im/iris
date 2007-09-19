CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = ../../bin

#DEFINES += IRISNET_STATIC
INCLUDEPATH += ../../include
include(../../src/irisnet/irisnet.pri)

SOURCES += main.cpp
