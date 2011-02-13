#include <gtest/gtest.h>
#include "reddnet.h"
#include <string>

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}



TEST( redd, init ) {
	int x = 0;
	x = x + 1;
	void * h;
	ASSERT_EQ( 0, redd_init()) << "Failed to init ReDDNet shim: " << redd_strerror();
	EXPECT_NE( h=redd_open("test_no_slashes", O_CREAT, S_IFREG), (void *)NULL )
		<< "Failed to open relative file without slashes: " << redd_strerror();
	EXPECT_EQ( 0,redd_close(h) ) << "Closing filehandle";
	EXPECT_NE( h=redd_open("/test_with_slashes", O_CREAT, S_IFREG), (void *)NULL )
		<< "Failed to open relative file with slashes: " << redd_strerror();
	EXPECT_EQ( 0, redd_close(h)) << "Closing filehandle";
	EXPECT_EQ( redd_open("/test_nonexistant_with_slashes", 0, S_IFREG), (void *)NULL )
		<< "Non-existant file was able to be opened";
	EXPECT_EQ( redd_open("test_nonexistant_no_slashes", 0, S_IFREG), (void *)NULL )
		<< "Non-existant file was able to be opened";
//	ASSERT_EQ( 0, redd_term()) << "Failed to destroy ReDDNet shim: " << redd_strerror();
}


TEST( redd, roundtrip ) {
	int x = 0;
	x = x + 1;
	void * h;
	// write some data in, then close, reopen and read it back out
	ASSERT_EQ( 0, redd_init()) << "Failed to init ReDDNet shim: " << redd_strerror();
	EXPECT_NE( h=redd_open("test_roundtrip", O_CREAT | O_TRUNC, S_IFREG), (void *)NULL )
		<< "Failed to open roundtrip for truncate" << redd_strerror();

	std::string testString("This is some text");
	EXPECT_EQ( redd_write(h, testString.c_str(), testString.size()),
				testString.size() ) << "Writing data into file";
	EXPECT_EQ( redd_close(h), 0 ) << "Closing filehandle";

	EXPECT_NE( h=redd_open("test_roundtrip", 0, S_IFREG), (void *)NULL )
		<< "Failed to open roundtrip" << redd_strerror();

	char buffer[1024];
	EXPECT_EQ( redd_read(h, buffer, testString.size()),
				testString.size() ) << "Reading data from file: " << redd_strerror();

	EXPECT_EQ( redd_close(h), 0 ) << "Closing filehandle";



//	ASSERT_EQ( 0, redd_term()) << "Failed to destroy ReDDNet shim: " << redd_strerror() << strerror(redd_errno());
}

