
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

enum ProcessState {
    STATE_UNKNOWN,
    STATE_RUNNING,
    STATE_STOPPED
};

static int launch_process(pid_t *pid, enum ProcessState *state)
{
    char *args[] = {"sleep", "300", NULL};

    pid_t child = fork();

    if (child < 0) {
        perror("fork");
        return -1;
    }

    if (child == 0) {
        execvp(args[0], args);
        perror("execvp");
        _exit(127);
    }

    *pid = child;
    *state = STATE_RUNNING;

    printf("Started test process. PID: %ld\n", (long)*pid);
    return 0;
}

static void check_process(pid_t *pid, enum ProcessState *state)
{
    int status;
    pid_t result;

    if (*pid <= 0) {
        printf("No active child process.\n");
        return;
    }

    while (1) {
        result = waitpid(
            *pid, &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result == 0)
            break;

        if (result < 0) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD) {
                printf("Child has exited or was already reaped.\n");
                *pid = -1;
                *state = STATE_UNKNOWN;
            } else {
                perror("waitpid");
            }
            return;
        }

        if (WIFSTOPPED(status)) {
            *state = STATE_STOPPED;
            printf("Child stopped by signal %d.\n",
                   WSTOPSIG(status));
        } else if (WIFCONTINUED(status)) {
            *state = STATE_RUNNING;
            printf("Child resumed.\n");
        } else if (WIFEXITED(status)) {
            printf("Child exited with status %d.\n",
                   WEXITSTATUS(status));
            *pid = -1;
            *state = STATE_UNKNOWN;
            return;
        } else if (WIFSIGNALED(status)) {
            printf("Child terminated by signal %d.\n",
                   WTERMSIG(status));
            *pid = -1;
            *state = STATE_UNKNOWN;
            return;
        }
    }

    if (*pid > 0) {
        printf("PID: %ld | Last observed state: ",
               (long)*pid);

        if (*state == STATE_STOPPED)
            printf("STOPPED\n");
        else if (*state == STATE_RUNNING)
            printf("RUNNING (last observed)\n");
        else
            printf("UNKNOWN\n");
    }
}

static int terminate_process(pid_t *pid,
                             enum ProcessState *state)
{
    int status;
    pid_t result;
    pid_t target = *pid;

    if (target <= 0) {
        printf("No active child to terminate.\n");
        return 0;
    }

    if (kill(target, SIGTERM) == -1) {
        if (errno == ESRCH) {
            printf("Process has already exited.\n");
        } else {
            perror("SIGTERM");
            return -1;
        }
    } else {
        printf("Termination requested for PID %ld.\n",
               (long)target);
    }

    /*
     * Also continue it in case it was stopped.
     * A pending SIGTERM can then take effect.
     */
    if (kill(target, SIGCONT) == -1 && errno != ESRCH) {
        perror("SIGCONT");
    }

    do {
        result = waitpid(target, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == target) {
        if (WIFSIGNALED(status)) {
            printf("Child terminated by signal %d.\n",
                   WTERMSIG(status));
        } else if (WIFEXITED(status)) {
            printf("Child exited with status %d.\n",
                   WEXITSTATUS(status));
        }
    } else if (result == -1 && errno != ECHILD) {
        perror("waitpid");
        return -1;
    }

    *pid = -1;
    *state = STATE_UNKNOWN;
    return 0;
}

static int send_signal_to_child(pid_t pid, int sig,
                                const char *action)
{
    if (pid <= 0) {
        printf("No active child. Use Restart to launch one.\n");
        return -1;
    }

    if (kill(pid, sig) == -1) {
        if (errno == ESRCH)
            printf("Process has already exited. Check status.\n");
        else
            perror("kill");
        return -1;
    }

    printf("%s signal sent to PID %ld.\n",
           action, (long)pid);
    return 0;
}

static int read_choice(void)
{
    char buffer[100];
    char *end;
    long value;

    if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        return -1;

    errno = 0;
    value = strtol(buffer, &end, 10);

    if (end == buffer || errno == ERANGE)
        return -2;

    while (*end == ' ' || *end == '\t' ||
           *end == '\n' || *end == '\r')
        end++;

    if (*end != '\0' || value < INT_MIN || value > INT_MAX)
        return -2;

    return (int)value;
}

int main(void)
{
    pid_t child_pid = -1;
    enum ProcessState state = STATE_UNKNOWN;
    int choice;

    printf("=== Linux Signal-Based Process Controller ===\n");
    printf("This controller manages its own sleep test process.\n\n");

    if (launch_process(&child_pid, &state) == -1)
        return EXIT_FAILURE;

    while (1) {
        printf("\n========== MENU ==========\n");
        printf("1. Pause process\n");
        printf("2. Resume process\n");
        printf("3. Terminate process\n");
        printf("4. Check process status\n");
        printf("5. Restart process\n");
        printf("0. Exit controller\n");
        printf("Enter choice: ");
        fflush(stdout);

        choice = read_choice();

        if (choice == -1) {
            printf("\nInput closed. Cleaning up...\n");
            break;
        }

        if (choice == -2) {
            printf("Invalid input. Enter a menu number.\n");
            continue;
        }

        /*
         * Collect any child-state changes before
         * processing the next menu command.
         */
        if (child_pid > 0)
            check_process(&child_pid, &state);

        switch (choice) {
            case 1:
                send_signal_to_child(
                    child_pid, SIGSTOP, "Pause (SIGSTOP)");
                break;

            case 2:
                send_signal_to_child(
                    child_pid, SIGCONT, "Resume (SIGCONT)");
                break;

            case 3:
                terminate_process(&child_pid, &state);
                break;

            case 4:
                if (child_pid > 0)
                    check_process(&child_pid, &state);
                else
                    printf("No active child process.\n");
                break;

            case 5:
                if (child_pid > 0 &&
                    terminate_process(&child_pid, &state) == -1) {
                    printf("Restart cancelled because cleanup failed.\n");
                    break;
                }

                if (launch_process(&child_pid, &state) == -1)
                    printf("Could not restart process.\n");
                break;

            case 0:
                printf("Exiting controller...\n");
                terminate_process(&child_pid, &state);
                return EXIT_SUCCESS;

            default:
                printf("Invalid choice. Choose 0 through 5.\n");
        }
    }

    terminate_process(&child_pid, &state);
    return EXIT_SUCCESS;
}
