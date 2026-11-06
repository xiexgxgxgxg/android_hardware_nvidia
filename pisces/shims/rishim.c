/*
 * ABI shim for the 2012 Spreadtrum RIL blob (libril-xiaomi-sprd.so).
 *
 * The blob was linked against the pre-Android-6 libcutils and references
 * socket_loopback_client(), which was removed when libcutils switched to the
 * socket_local_* API.  Without it rild dies at dlopen time:
 *
 *   dlopen failed: cannot locate symbol "socket_loopback_client" referenced by
 *   "/system/lib/libril-xiaomi-sprd.so"
 *
 * LD_PRELOADed into the ril-daemon service.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

int socket_loopback_client(int port, int type)
{
    struct sockaddr_in addr;
    int s;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    s = socket(AF_INET, type, 0);
    if (s < 0)
        return -1;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }

    return s;
}
