TEMPLATE = subdirs

include(build.pri)

appledns:!appledns_bundle:CONFIG *= build_src
!irisnetcore_bundle:CONFIG *= build_src

build_src:SUBDIRS += src
SUBDIRS += tools
