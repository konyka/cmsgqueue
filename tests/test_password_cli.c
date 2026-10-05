#define _POSIX_C_SOURCE 200809L
#include "cmq_test.h"
#include "cmq_password.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CMQ_PASSWORD_CLI_PATH
#error "CMQ_PASSWORD_CLI_PATH must name the built cmq-password executable"
#endif

typedef struct {
    int status;
    int runner_error;
    size_t output_len;
    char output[512];
    size_t error_len;
    char error[512];
} cli_result_t;

static cli_result_t run_cli(const unsigned char *input, size_t input_len,
                            int argc, char *const argv[]) {
    cli_result_t result = {.status = -1, .runner_error = 1};
    (void)argc;
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    struct sigaction old_pipe;
    int pipe_ignored = 0;
    sigemptyset(&ignore.sa_mask);
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
        goto cleanup;

    pid_t pid = fork();
    if (pid == 0) {
        if (dup2(in_pipe[0], STDIN_FILENO) < 0 ||
            dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(err_pipe[1], STDERR_FILENO) < 0)
            _exit(126);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execv(CMQ_PASSWORD_CLI_PATH, argv);
        _exit(127);
    }
    if (pid < 0)
        goto cleanup;

    close(in_pipe[0]); in_pipe[0] = -1;
    close(out_pipe[1]); out_pipe[1] = -1;
    close(err_pipe[1]); err_pipe[1] = -1;
    if (sigaction(SIGPIPE, &ignore, &old_pipe) != 0) {
        result.runner_error = 1;
    } else {
        result.runner_error = 0;
        pipe_ignored = 1;
    }
    size_t written = 0;
    while (written < input_len && !result.runner_error) {
        ssize_t n = write(in_pipe[1], input + written, input_len - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            result.runner_error = 1;
            break;
        }
        written += (size_t)n;
    }
    close(in_pipe[1]); in_pipe[1] = -1;

    while (result.output_len < sizeof(result.output) - 1) {
        ssize_t n = read(out_pipe[0], result.output + result.output_len,
                         sizeof(result.output) - 1 - result.output_len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        result.output_len += (size_t)n;
    }
    while (result.error_len < sizeof(result.error) - 1) {
        ssize_t n = read(err_pipe[0], result.error + result.error_len,
                         sizeof(result.error) - 1 - result.error_len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        result.error_len += (size_t)n;
    }
    close(out_pipe[0]); out_pipe[0] = -1;
    close(err_pipe[0]); err_pipe[0] = -1;
    if (pipe_ignored && sigaction(SIGPIPE, &old_pipe, NULL) != 0) {
        fprintf(stderr, "failed to restore SIGPIPE disposition\n");
        abort();
    }
    for (;;) {
        if (waitpid(pid, &result.status, 0) >= 0) break;
        if (errno != EINTR) {
            result.runner_error = 1;
            break;
        }
    }
    result.output[result.output_len] = '\0';
    result.error[result.error_len] = '\0';
    return result;

cleanup:
    if (in_pipe[0] >= 0) close(in_pipe[0]);
    if (in_pipe[1] >= 0) close(in_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    result.output[0] = '\0';
    result.error[0] = '\0';
    return result;
}

static int exit_code(const cli_result_t *result) {
    return !result->runner_error && WIFEXITED(result->status) ? WEXITSTATUS(result->status) : 255;
}

static void assert_success_hash(cli_result_t *result,
                                const char *password) {
    ASSERT_EQ(exit_code(result), 0);
    ASSERT_EQ(result->output_len > 0, 1);
    ASSERT_EQ(result->output[result->output_len - 1], '\n');
    ASSERT_EQ(result->output_len < sizeof(result->output), 1);
    result->output[result->output_len - 1] = '\0';
    ASSERT_EQ(cmq_password_verify(result->output, password), 1);
}

TEST(password_cli, hashes_stdin_without_newline_and_preserves_spaces) {
    static const unsigned char input[] = " leading and trailing spaces ";
    char *argv[] = { (char *)"cmq-password", NULL };
    cli_result_t result = run_cli(input, sizeof(input) - 1, 1, argv);
    assert_success_hash(&result, (const char *)input);
}

TEST(password_cli, strips_one_lf_or_crlf_only) {
    static const unsigned char lf[] = "secret\n";
    static const unsigned char crlf[] = "secret\r\n";
    char *argv[] = { (char *)"cmq-password", NULL };
    cli_result_t first = run_cli(lf, sizeof(lf) - 1, 1, argv);
    cli_result_t second = run_cli(crlf, sizeof(crlf) - 1, 1, argv);
    assert_success_hash(&first, "secret");
    assert_success_hash(&second, "secret");
}

TEST(password_cli, accepts_255_bytes_with_crlf) {
    unsigned char input[257];
    char expected[256];
    char *argv[] = { (char *)"cmq-password", NULL };
    memset(input, 'p', 255);
    input[255] = '\r';
    input[256] = '\n';
    memset(expected, 'p', sizeof(expected) - 1);
    expected[sizeof(expected) - 1] = '\0';
    cli_result_t result = run_cli(input, sizeof(input), 1, argv);
    assert_success_hash(&result, expected);
}

TEST(password_cli, rejects_empty_input_and_overbuffer_input) {
    unsigned char oversized[CMQ_PASSWORD_MAX + 4];
    char *argv[] = { (char *)"cmq-password", NULL };
    memset(oversized, 'x', sizeof(oversized));
    cli_result_t empty = run_cli(NULL, 0, 1, argv);
    cli_result_t oversized_result = run_cli(oversized, sizeof(oversized), 1, argv);
    ASSERT(exit_code(&empty) != 0);
    ASSERT_EQ(empty.output_len, 0);
    ASSERT(exit_code(&oversized_result) != 0);
    ASSERT_EQ(oversized_result.output_len, 0);
}

TEST(password_cli, help_is_available_without_hashing) {
    char *argv[] = { (char *)"cmq-password", (char *)"--help", NULL };
    cli_result_t result = run_cli(NULL, 0, 2, argv);
    ASSERT_EQ(exit_code(&result), 0);
    ASSERT_STR_EQ(result.output, "Usage: cmq-password < password-file\n");
}

TEST(password_cli, fresh_hashes_use_random_salts) {
    static const unsigned char input[] = "same password";
    char *argv[] = { (char *)"cmq-password", NULL };
    cli_result_t first = run_cli(input, sizeof(input) - 1, 1, argv);
    cli_result_t second = run_cli(input, sizeof(input) - 1, 1, argv);
    ASSERT_EQ(exit_code(&first), 0);
    ASSERT_EQ(exit_code(&second), 0);
    ASSERT(first.output_len > 1);
    ASSERT(second.output_len > 1);
    ASSERT(memcmp(first.output, second.output, first.output_len) != 0);
    first.output[first.output_len - 1] = '\0';
    second.output[second.output_len - 1] = '\0';
    ASSERT_EQ(cmq_password_verify(first.output, (const char *)input), 1);
    ASSERT_EQ(cmq_password_verify(second.output, (const char *)input), 1);
}

TEST(password_cli, accepts_255_bytes_but_rejects_256_bytes) {
    unsigned char valid[255], oversized[256];
    memset(valid, 'x', sizeof(valid));
    memset(oversized, 'x', sizeof(oversized));
    char *argv[] = { (char *)"cmq-password", NULL };
    cli_result_t accepted = run_cli(valid, sizeof(valid), 1, argv);
    cli_result_t rejected = run_cli(oversized, sizeof(oversized), 1, argv);
    char expected[256];
    memset(expected, 'x', sizeof(expected) - 1);
    expected[sizeof(expected) - 1] = '\0';
    assert_success_hash(&accepted, expected);
    ASSERT(exit_code(&rejected) != 0);
    ASSERT_EQ(rejected.output_len, 0);
}

TEST(password_cli, rejects_nul_and_remaining_line_breaks) {
    static const unsigned char nul[] = { 'a', '\0', 'b' };
    static const unsigned char multiline[] = "first\nsecond";
    static const unsigned char trailing_cr[] = "secret\r";
    char *argv[] = { (char *)"cmq-password", NULL };
    cli_result_t nul_result = run_cli(nul, sizeof(nul), 1, argv);
    cli_result_t multiline_result = run_cli(multiline, sizeof(multiline) - 1, 1, argv);
    cli_result_t cr_result = run_cli(trailing_cr, sizeof(trailing_cr) - 1, 1, argv);
    ASSERT(exit_code(&nul_result) != 0);
    ASSERT(exit_code(&multiline_result) != 0);
    ASSERT(exit_code(&cr_result) != 0);
    ASSERT_EQ(nul_result.output_len, 0);
    ASSERT_EQ(multiline_result.output_len, 0);
    ASSERT_EQ(cr_result.output_len, 0);
}

TEST(password_cli, rejects_arguments_without_disclosing_them) {
    static const unsigned char input[] = "safe";
    char *argv[] = { (char *)"cmq-password", (char *)"secret-argument", NULL };
    cli_result_t result = run_cli(input, sizeof(input) - 1, 2, argv);
    ASSERT(exit_code(&result) != 0);
    ASSERT_EQ(result.output_len, 0);
    ASSERT(strstr(result.error, "secret-argument") == NULL);
}

TEST_MAIN()
