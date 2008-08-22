IRIS_BASE = $$PWD/..

unix:include(../conf.pri)
windows:include(../conf_win.pri)

!isEmpty(IRIS_BUILDLIB_PRI):include($$IRIS_BASE/$$IRIS_BUILDLIB_PRI)
include(../common.pri)

QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.3
