#include "http-client.h"
#include <unistd.h>

int main(void) {
    curl_init("http://localhost/", 1337);
    asynch_send("test.bin", 0, 500000, "testdir/output.bin");
    sleep(5);
    asynch_send("test.bin", 10000000, 500000, "anotherdir/bn.bin");
    sleep(5);
    asynch_send("test.bin", 40000000, 500000, "anotherdir/bn.bin");
    sleep(5);
    return curl_destroy();
}
