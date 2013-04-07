/*
 * Copyright (C) 2008  Remko Troncon
 * See COPYING for license details.
 */

#include <QCoreApplication>

#include "qttestutil/testregistry.h"

/**
 * Runs all tests registered with the QtTestUtil registry.
 */
int main(int argc, char* argv[])
{
	QCoreApplication application(argc, argv);
	return QtTestUtil::TestRegistry::getInstance()->runTests(argc, argv);
}

#ifdef QCA_STATIC
#include <QtPlugin>
#ifdef HAVE_OPENSSL
Q_IMPORT_PLUGIN(qca_ossl)
#endif
#endif
