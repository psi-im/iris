TEMPLATE = subdirs

include(../build.pri)

SUBDIRS += irisnet
!iris_bundle:SUBDIRS += xmpp
