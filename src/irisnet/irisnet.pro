TEMPLATE = subdirs

include(../../build.pri)

appledns:!appledns_bundle:SUBDIRS += appledns
!irisnetcore_bundle:SUBDIRS += corelib
!iris_bundle:SUBDIRS += noncore
