#define _POSIX_C_SOURCE 200809L
#include "cmq_password.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* Raw input may contain a 255-byte password plus CRLF and a terminator. */
#define CMQ_PASSWORD_CLI_MAX (CMQ_PASSWORD_MAX - 1)
#define CMQ_PASSWORD_INPUT_CAP (CMQ_PASSWORD_CLI_MAX + 3)

static int finish(unsigned char *password, char *hash, int status) {
    OPENSSL_cleanse(password, CMQ_PASSWORD_INPUT_CAP);
    OPENSSL_cleanse(hash, CMQ_PASSWORD_MAX);
    return status;
}

static volatile sig_atomic_t interrupted;

static void handle_interrupt(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

int main(int argc, char **argv) {
    unsigned char password[CMQ_PASSWORD_INPUT_CAP] = {0};
    char hash[CMQ_PASSWORD_MAX] = {0};
    size_t len = 0;
    int ch;
    struct sigaction action = {0};
    struct sigaction ignore_pipe = {0};

    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGPIPE, &ignore_pipe, NULL) != 0)
        return finish(password, hash, 1);

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        if (fputs("Usage: cmq-password < password-file\n", stdout) == EOF ||
            fflush(stdout) != 0)
            return finish(password, hash, 1);
        return finish(password, hash, 0);
    }
    if (argc != 1 || isatty(STDIN_FILENO))
        return finish(password, hash, 1);

    while (!interrupted && (ch = getchar()) != EOF) {
        if (len >= sizeof(password) - 1)
            return finish(password, hash, 1);
        password[len++] = (unsigned char)ch;
    }
    if (interrupted || ferror(stdin) || len == 0)
        return finish(password, hash, 1);

    if (password[len - 1] == '\n') {
        --len;
        if (len > 0 && password[len - 1] == '\r')
            --len;
    }
    if (len == 0 || len > CMQ_PASSWORD_CLI_MAX)
        return finish(password, hash, 1);
    password[len] = '\0';
    for (size_t i = 0; i < len; ++i) {
        if (password[i] == '\0' || password[i] == '\r' || password[i] == '\n')
            return finish(password, hash, 1);
    }

    if (cmq_password_hash((const char *)password, hash, sizeof(hash)) != 0)
        return finish(password, hash, 1);
    if (interrupted)
        return finish(password, hash, 1);
    if (fputs(hash, stdout) == EOF || fputc('\n', stdout) == EOF ||
        fflush(stdout) != 0)
        return finish(password, hash, 1);
    return finish(password, hash, 0);
}
