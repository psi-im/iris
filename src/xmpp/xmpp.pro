IRIS_BASE = ../..

TEMPLATE = lib
#QT      -= gui
TARGET   = iris
DESTDIR  = $$IRIS_BASE/lib
CONFIG  += staticlib create_prl

VERSION = 1.0.0

CONFIG += crypto

include(xmpp.pri)
