include(../../modules.pri)
include($$IRIS_XMPP_QA_UNITTEST_MODULE)
include($$IRIS_XMPP_SASL_MODULE)
include($$IRIS_XMPP_BASE_MODULE)

HEADERS += \
	$$PWD/plainmessagetest.h \
	$$PWD/digestmd5responsetest.h

# QCA stuff.
# TODO: Remove the dependency on QCA from this module
include(../../../../../third-party/qca/qca.pri)
