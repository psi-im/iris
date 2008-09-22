#include "plainmessagetest.h"
#include "digestmd5responsetest.h"
#include "unittest.h"

#include <QtCrypto>

int main(int argc, char* argv[])
{
	QCoreApplication app(argc, argv);
	QCA::Initializer initializer;

	BEGIN_UNITTESTS;
	RUN_UNITTEST(PlainMessageTest);
	RUN_UNITTEST(DIGESTMD5ResponseTest);
	END_UNITTESTS;
}
