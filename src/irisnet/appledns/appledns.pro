IRIS_BASE = ../../..

TEMPLATE = lib
CONFIG += plugin
QT -= gui
DESTDIR = $$IRIS_BASE/plugins

VERSION = 1.0.0

INCLUDEPATH *= $$PWD/../corelib
include(appledns.pri)
