#include "randomnumbergeneratortest.h"
#include "randrandomnumbergeneratortest.h"
#include "unittest.h"

int main(int argc, char* argv[])
{
	BEGIN_UNITTESTS;
	RUN_UNITTEST(RandomNumberGeneratorTest);
	RUN_UNITTEST(RandRandomNumberGeneratorTest);
	END_UNITTESTS;
}
