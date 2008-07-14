TEMPLATE = subdirs

include(common.pri)

# do we have a reason to enter the src dir?
appledns:!appledns_bundle:CONFIG *= build_src
!irisnetcore_bundle:CONFIG *= build_src
!iris_bundle:CONFIG *= build_src

build_src:SUBDIRS += src
SUBDIRS += tools
