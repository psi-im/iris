IRIS_BASE = $$PWD/..

unix:include(../conf.pri)
windows:include(../conf_win.pri)

# HACK: fish into psi for iris config
include(../../src/conf_iris.pri)

!isEmpty(IRIS_BUILDLIB_PRI):include($$IRIS_BASE/$$IRIS_BUILDLIB_PRI)
include(../common.pri)

QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.3
