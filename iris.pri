IRIS_BASE = $$PWD
include(common.pri)

INCLUDEPATH += $$IRIS_BASE/include $$IRIS_BASE/include/iris

iris_bundle:{
	include(src/xmpp/xmpp.pri)
}
else {
	LIBS += -L$$IRIS_BASE/lib -liris
}
