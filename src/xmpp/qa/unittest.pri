#
# Declares all common settings/includes for a unittest module.
#
# Include this file from your module's unittest project file to create a 
# standalone checker for the module.
#

CONFIG += qca-static
DEFINES += HAVE_OPENSSL
# use qca from psi if necessary
qca-static {
	DEFINES += QCA_STATIC
	DEFINES += QT_STATICPLUGIN
	INCLUDEPATH += $$PWD/../../../../third-party/qca/qca/include/QtCrypto

	include($$PWD/../../../../third-party/qca/qca.pri)

	# QCA-OpenSSL
	contains(DEFINES, HAVE_OPENSSL) {
		include($$PWD/../../../../third-party/qca/qca-ossl.pri)
	}
}
else {
	CONFIG += crypto
}

include($$PWD/qttestutil/qttestutil.pri)
include($$PWD/../common.pri)

QT += testlib
QT -= gui
CONFIG -= app_bundle

INCLUDEPATH *= $$PWD
DEPENDPATH *= $$PWD

TARGET = checker

SOURCES += \
	$$PWD/qttestutil/simplechecker.cpp

QMAKE_EXTRA_TARGETS = check
check.commands = \$(MAKE) && ./checker

QMAKE_CLEAN += $(QMAKE_TARGET)
