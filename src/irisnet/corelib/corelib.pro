IRIS_BASE = ../../..

TEMPLATE = lib
QT      -= gui
TARGET   = irisnetcore
DESTDIR  = $$IRIS_BASE/lib
windows:DLLDESTDIR = $$IRIS_BASE/bin

VERSION = 1.0.0

HEADERS += \
	jdnsshared.h

SOURCES += \
	jdnsshared.cpp
