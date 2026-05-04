#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

static void default_socket_path(char *out, size_t out_len) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0]) {
        snprintf(out, out_len, "%s/mc-resizer.sock", runtime);
    } else {
        snprintf(out, out_len, "/tmp/mc-resizer-%ld.sock", (long)getuid());
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [--socket PATH] thin|wide|full|cycle|center|status|rescan|quit\n",
            argv0);
}

int main(int argc, char **argv) {
    char sock_path[UNIX_PATH_MAX];
    const char *command = NULL;
    default_socket_path(sock_path, sizeof(sock_path));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            snprintf(sock_path, sizeof(sock_path), "%s", argv[i]);
        } else if (!command) {
            command = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!command) {
        usage(argv[0]);
        return 2;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    char line[128];
    int len = snprintf(line, sizeof(line), "%s\n", command);
    if (write(fd, line, (size_t)len) != len) {
        perror("write");
        close(fd);
        return 1;
    }

    char reply[512];
    ssize_t n = read(fd, reply, sizeof(reply) - 1);
    if (n > 0) {
        reply[n] = '\0';
        fputs(reply, stdout);
    }

    close(fd);
    return 0;
}
