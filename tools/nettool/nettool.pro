CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = ../../bin

INCLUDEPATH += ../../include
include(../../src/irisnet/noncore/noncore.pri)

SOURCES += main.cpp
