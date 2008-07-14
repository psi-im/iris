TEMPLATE = subdirs

include(../../common.pri)

!irisnetcore_bundle:SUBDIRS += corelib
appledns:!appledns_bundle:SUBDIRS += appledns
!iris_bundle:SUBDIRS += noncore
