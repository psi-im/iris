# common stuff for iris.pro and iris.pri

include(build.pri)

# HACK: check for psi configuration
unix:include(../conf.pri)
windows:include(../conf_windows.pri)

# HACK: use qca from psi if necessary
qca-static {
	DEFINES += QCA_STATIC
	INCLUDEPATH += $$PWD/../third-party/qca/qca/include/QtCrypto
}

# HACK: use zlib from psi if necessary
psi-zip {
	INCLUDEPATH += $$PWD/../src/tools/zip/minizip/win32
}

# HACK: this should not be in common.pri, since it means the user of iris gets
#  this code as well
mac {
	# Universal binaries
	qc_universal:contains(QT_CONFIG,x86):contains(QT_CONFIG,ppc) {
		CONFIG += x86 ppc
		QMAKE_MAC_SDK=/Developer/SDKs/MacOSX10.4u.sdk
		QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.4
	}
}

# default build configuration
!iris_build_pri {
	# build appledns on mac
	mac:CONFIG += appledns

	# bundle appledns inside of irisnetcore on mac
	mac:CONFIG += appledns_bundle

	# bundle irisnetcore inside of iris
	CONFIG += irisnetcore_bundle

	# don't build iris, app will include iris.pri
	#CONFIG += iris_bundle
}
