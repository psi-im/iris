QT += testlib
QT -= gui
CONFIG -= app_bundle

MOC_DIR = .moc
OBJECTS_DIR = .obj

INCLUDEPATH *= $$PWD
DEPENDPATH *= $$PWD

TARGET = checker

SOURCES += \
	checker.cpp

QMAKE_EXTRA_TARGETS = check
check.commands = \$(MAKE) && ./checker

QMAKE_CLEAN += $(QMAKE_TARGET)
