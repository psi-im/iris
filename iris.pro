TEMPLATE = subdirs

IRIS_BASE = $$PWD

unix:include(conf.pri)
windows:include(conf_win.pri)

# HACK: fish into psi for iris config
include(../src/conf_iris.pri)

!isEmpty(IRIS_BUILDLIB_PRI):include($$IRIS_BASE/$$IRIS_BUILDLIB_PRI)
include(common.pri)

# do we have a reason to enter the src dir?
appledns:!appledns_bundle:CONFIG *= build_src
!irisnetcore_bundle:CONFIG *= build_src
!iris_bundle:CONFIG *= build_src

build_src:SUBDIRS += src

!disable_tests:SUBDIRS += tools
