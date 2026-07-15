/*
 * =============================================================================
 *  sdsh — Super Duper Shell (DOS Edition)
 *  Version: 2.0
 *  Target:  Ubuntu / KDE Neon (Konsole terminal)
 *
 *  Build:
 *    gcc -Wall -Wextra -o sdsh sdsh.c
 *
 *  Usage (interactive):
 *    ./sdsh
 *
 *  Usage (scripting):
 *    ./sdsh script.sdsh
 *
 *  Features
 *  --------
 *  • Classic MS-DOS look:   blue screen, gray/white text, C:\PATH> prompt
 *  • Command translation:
 *      File ops:    dir→ls  vol→lsblk  md→mkdir  rd→rmdir  del/erase→rm
 *                   copy→cp  move→mv  ren/rename→mv  type→cat
 *      System ops:  grab→apt install  refresh→apt update  evolve→apt upgrade
 *                   revive→reboot  sleep→poweroff
 *      Privilege:   superuser <cmd> → sudo <cmd>
 *  • Built-ins:             about, ver, cls, cd, whereami, exit
 *  • External pass-through: execvp() for everything else
 *  • Script execution:      reads & runs .sdsh files line-by-line
 *  • Signal handling:       Ctrl+C clears the line instead of killing sdsh
 *  • Graceful error msgs:   unknown commands, fork failures, etc.
 * =============================================================================
 */

/* ── POSIX / standard headers ─────────────────────────────────────────────── */
#define _POSIX_C_SOURCE 200809L   /* enable POSIX APIs (getline, sigaction…) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       /* fork, execvp, chdir, getcwd, getuid             */
#include <sys/types.h>    /* pid_t                                            */
#include <sys/wait.h>     /* wait, waitpid                                    */
#include <signal.h>       /* sigaction, SIGINT                                */
#include <errno.h>        /* errno, strerror                                  */
#include <pwd.h>          /* getpwuid – resolves username from UID            */
#include <ctype.h>        /* isspace                                          */

/* ── Compile-time constants ───────────────────────────────────────────────── */
#define SDSH_VERSION     "2.0"
#define SDSH_MAX_ARGS    128      /* max tokens in a single command line      */
#define SDSH_MAX_LINE    4096     /* max characters per input line            */
#define SDSH_CWD_MAX     1024     /* max path length shown in prompt          */
#define SDSH_DOS_MAX     (SDSH_CWD_MAX + 4)  /* room for the "C:" prefix      */

/* ── ANSI color codes (Konsole / any VT100-compatible terminal) ───────────── */
#define COL_RESET        "\033[0m"
#define COL_BOLD         "\033[1m"
#define COL_GREEN        "\033[38;5;82m"   /* bright green  – unused now       */
#define COL_CYAN         "\033[38;5;87m"   /* sky cyan      – unused now       */
#define COL_YELLOW       "\033[38;5;220m"  /* golden yellow – unused now       */
#define COL_RED          "\033[38;5;196m"  /* vivid red     – error messages  */
#define COL_MAGENTA      "\033[38;5;213m"  /* soft magenta  – about art       */
#define COL_BLUE         "\033[38;5;75m"   /* cornflower    – about info text */

/* ── Classic MS-DOS "blue screen" palette ─────────────────────────────────── */
#define COL_BG_DOS       "\033[44m"        /* blue background  (color 1)      */
#define COL_FG_DOS       "\033[37m"        /* light gray text  (color 7)      */
#define COL_FG_DOS_HI    "\033[97m"        /* bright white     – prompt path  */
#define CLR_SCREEN       "\033[2J\033[H"   /* clear + home cursor             */

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void  sdsh_sigint_handler(int sig);
static void  sdsh_to_dos_path(const char *unix_path, char *out, size_t out_sz);
static void  sdsh_print_prompt(void);
static void  sdsh_print_about(void);
static char *sdsh_trim(char *s);
static int   sdsh_tokenize(char *line, char **argv, int max_args);
static const char *sdsh_translate(const char *cmd);
static int   sdsh_run_builtin(char **argv, int argc);
static void  sdsh_run_external(char **argv);
static void  sdsh_execute(char *line);
static void  sdsh_run_script(const char *path);
static void  sdsh_repl(void);

/* ── Global flag: set by SIGINT handler so the REPL can redraw the prompt ── */
static volatile sig_atomic_t g_got_sigint = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Signal handling
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_sigint_handler
 * -------------------
 * Called when the user presses Ctrl+C.
 * We just set a flag and write a newline; the REPL loop handles the rest.
 * Using write() here is async-signal-safe (printf is not).
 */
static void sdsh_sigint_handler(int sig)
{
    (void)sig;                         /* suppress unused-parameter warning   */
    g_got_sigint = 1;
    /* Move to a fresh line so the next prompt looks clean */
    const char msg[] = "\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Prompt
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_to_dos_path
 * ----------------
 * Converts a Unix-style path (e.g. "/home/kyle/project") into a faux
 * DOS path on a "C:" drive (e.g. "C:\HOME\KYLE\PROJECT").
 *
 * Mapping is purely cosmetic: the real filesystem root "/" becomes the
 * drive root "C:\", forward slashes become backslashes, and letters are
 * upper-cased to match classic DOS output. No 8.3 filename truncation
 * is applied — this is a look, not a real DOS filesystem.
 */
static void sdsh_to_dos_path(const char *unix_path, char *out, size_t out_sz)
{
    size_t oi = 0;

    /* Leave room for the trailing NUL */
    if (out_sz < 3) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }

    out[oi++] = 'C';
    out[oi++] = ':';

    for (const char *p = unix_path; *p && oi < out_sz - 1; p++) {
        char c = *p;
        if (c == '/')
            c = '\\';
        else
            c = (char)toupper((unsigned char)c);
        out[oi++] = c;
    }

    /* A bare "/" produces just "C:\" already; nothing extra needed. */
    out[oi] = '\0';
}

/*
 * sdsh_print_prompt
 * -----------------
 * Prints a classic MS-DOS style prompt:  C:\PATH>
 * Runs on the standing blue-screen background set up in main().
 */
static void sdsh_print_prompt(void)
{
    /* ── Current working directory, translated to a DOS-style path ───────── */
    char cwd[SDSH_CWD_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        strncpy(cwd, "/", sizeof(cwd));

    char dos_path[SDSH_DOS_MAX];
    sdsh_to_dos_path(cwd, dos_path, sizeof(dos_path));

    /* ── Render prompt ──────────────────────────────────────────────────── */
    printf(COL_BOLD COL_FG_DOS_HI "%s" COL_RESET COL_FG_DOS ">" COL_RESET " ",
           dos_path);

    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  About / ASCII art
 * ═══════════════════════════════════════════════════════════════════════════ */

static void sdsh_print_about(void)
{
    printf(COL_BOLD COL_FG_DOS_HI
        "\n"
        "   ███████╗██████╗ ███████╗██╗  ██╗\n"
        "   ██╔════╝██╔══██╗██╔════╝██║  ██║\n"
        "   ███████╗██║  ██║███████╗███████║\n"
        "   ╚════██║██║  ██║╚════██║██╔══██║\n"
        "   ███████║██████╔╝███████║██║  ██║\n"
        "   ╚══════╝╚═════╝ ╚══════╝╚═╝  ╚═╝\n"
        "      Super Duper Shell — DOS Edition v" SDSH_VERSION "\n"
        COL_RESET
        COL_FG_DOS
        "\n"
        "   A lightweight custom shell for Ubuntu & KDE Neon,\n"
        "   dressed up in classic MS-DOS clothing.\n"
        "   Talks directly to the Linux kernel (fork/exec/wait).\n"
        "\n"
        "   ── File & Directory Commands ──────────────────────────\n"
        "     dir                     →  ls\n"
        "     vol                     →  lsblk\n"
        "     md        <dir>         →  mkdir\n"
        "     rd        <dir>         →  rmdir\n"
        "     del / erase <target>    →  rm\n"
        "     copy      <src> <dst>   →  cp\n"
        "     move      <src> <dst>   →  mv\n"
        "     ren / rename <a> <b>    →  mv\n"
        "     type      <file>        →  cat\n"
        "\n"
        "   ── System & App Management ────────────────────────────\n"
        "     grab      <package>     →  apt install\n"
        "     refresh                 →  apt update\n"
        "     evolve                  →  apt upgrade\n"
        "     revive                  →  reboot\n"
        "     sleep                   →  poweroff\n"
        "\n"
        "   ── Privilege Escalation ───────────────────────────────\n"
        "     superuser <command>     →  sudo <command>\n"
        "     (sdsh aliases work too: superuser grab vim)\n"
        "\n"
        "   ── Built-in Commands ──────────────────────────────────\n"
        "     about   ver   whereami   cls   cd   exit\n"
        COL_RESET "\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  String helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_trim
 * ---------
 * Strips leading and trailing whitespace in-place; returns the new start.
 */
static char *sdsh_trim(char *s)
{
    if (!s) return s;

    /* Skip leading whitespace */
    while (*s && isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    /* Strip trailing whitespace by overwriting with NUL */
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    *(end + 1) = '\0';

    return s;
}

/*
 * sdsh_tokenize
 * -------------
 * Splits `line` into whitespace-delimited tokens stored in `argv`.
 * Returns the token count (argc).  argv is NULL-terminated.
 *
 * NOTE: modifies `line` in-place (strtok inserts NUL bytes).
 */
static int sdsh_tokenize(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *token = strtok(line, " \t\r\n");
    while (token && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;   /* execvp requires a NULL sentinel */
    return argc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Command translation layer
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_translate
 * --------------
 * Maps sdsh custom command names to their real Linux binary equivalents.
 * Returns the translated name, or the original if no mapping exists.
 */
static const char *sdsh_translate(const char *cmd)
{
    /*
     * Translation table – two logical sections:
     *
     *  FILE & DIRECTORY OPERATIONS (classic MS-DOS command names)
     *    dir           → ls          (directory listing)
     *    vol           → lsblk       (DOS "vol" shows volume info; closest
     *                                  Linux analogue is a block-device list)
     *    md            → mkdir       (make directory)
     *    rd            → rmdir       (remove an empty directory)
     *    del / erase   → rm          (remove files/dirs)
     *    copy          → cp          (copy files)
     *    move          → mv          (move files)
     *    ren / rename  → mv          (rename files)
     *    type          → cat         (view file contents)
     *
     *  SYSTEM & APP MANAGEMENT
     *    grab      → apt          (install packages – user passes "install pkg")
     *    refresh   → apt          (update package lists – shell appends "update")
     *    evolve    → apt          (upgrade packages  – shell appends "upgrade")
     *    revive    → reboot       (reboot the machine)
     *    sleep     → poweroff     (shut the machine down)
     *
     * Note: grab/refresh/evolve are special-cased below because they need
     * argv manipulation, not just a name swap.
     */

    /* ── Simple 1-to-1 translations ────────────────────────────────────── */
    static const struct { const char *from; const char *to; } table[] = {
        /* file ops — DOS command names */
        { "dir",       "ls"       },
        { "vol",       "lsblk"    },
        { "md",        "mkdir"    },
        { "rd",        "rmdir"    },
        { "del",       "rm"       },
        { "erase",     "rm"       },
        { "copy",      "cp"       },
        { "move",      "mv"       },
        { "ren",       "mv"       },
        { "rename",    "mv"       },
        { "type",      "cat"      },
        /* system ops */
        { "revive",    "reboot"   },
        { "sleep",     "poweroff" },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(cmd, table[i].from) == 0)
            return table[i].to;
    }
    return cmd;   /* not a custom command; pass through unchanged */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Built-in commands
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_run_builtin
 * ----------------
 * Handles commands that must run inside the shell process itself
 * (cd, exit, about, cls).
 *
 * Returns 1 if the command was handled as a built-in, 0 otherwise.
 * Sets *exit_requested to 1 when "exit" is typed.
 */

/* We use a static flag instead of a pointer parameter to keep signatures
 * simple across the call chain.                                             */
static int g_exit_requested = 0;

static int sdsh_run_builtin(char **argv, int argc)
{
    if (argc == 0 || argv[0] == NULL)
        return 0;

    const char *cmd = argv[0];

    /* ── exit ───────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "exit") == 0) {
        printf(COL_BOLD COL_FG_DOS_HI "\nGoodbye from sdsh.\n" COL_RESET);
        g_exit_requested = 1;
        return 1;
    }

    /* ── about ──────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "about") == 0) {
        sdsh_print_about();
        return 1;
    }

    /* ── ver (classic DOS version banner) ─────────────────────────────────── */
    if (strcmp(cmd, "ver") == 0) {
        printf(COL_BOLD COL_FG_DOS_HI
               "\nsdsh — Super Duper Shell, DOS Edition\n"
               "Version " SDSH_VERSION "\n\n" COL_RESET);
        return 1;
    }

    /* ── cls (clear screen) ─────────────────────────────────────────────── */
    if (strcmp(cmd, "cls") == 0) {
        /*
         * ANSI escape: ESC[H moves cursor to top-left,
         *              ESC[2J clears the screen.
         * Using write() directly avoids buffering artefacts.
         */
        write(STDOUT_FILENO, "\033[H\033[2J", 7);
        return 1;
    }

    /* ── cd ─────────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "cd") == 0) {
        const char *target;

        if (argc < 2 || argv[1] == NULL) {
            /* No argument → go to $HOME */
            target = getenv("HOME");
            if (!target) {
                struct passwd *pw = getpwuid(getuid());
                target = pw ? pw->pw_dir : "/";
            }
        } else {
            target = argv[1];
        }

        if (chdir(target) != 0) {
            fprintf(stderr,
                COL_RED "sdsh: cd: %s: %s\n" COL_RESET,
                target, strerror(errno));
        }
        return 1;
    }

    /* ── whereami (pwd) ─────────────────────────────────────────────────── */
    if (strcmp(cmd, "whereami") == 0) {
        char cwd[SDSH_CWD_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            char dos_path[SDSH_DOS_MAX];
            sdsh_to_dos_path(cwd, dos_path, sizeof(dos_path));
            printf(COL_BOLD COL_FG_DOS_HI "%s\n" COL_RESET, dos_path);
        } else {
            fprintf(stderr,
                COL_RED "sdsh: whereami: %s\n" COL_RESET,
                strerror(errno));
        }
        return 1;
    }

    return 0;   /* not a built-in */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  External command execution
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_run_external
 * -----------------
 * Forks a child process and uses execvp() to run the command.
 * The parent waits for the child to finish.
 *
 * execvp() searches PATH automatically, so we don't need to locate
 * the binary ourselves.
 */
static void sdsh_run_external(char **argv)
{
    /*
     * Flush any buffered output from prior builtins (about, ver, whereami…)
     * before the child starts writing directly to the same fd. Without
     * this, output can appear out of order whenever stdout isn't a TTY
     * (e.g. piped, redirected to a file, or run via script mode).
     */
    fflush(stdout);

    pid_t pid = fork();

    if (pid < 0) {
        /* fork() itself failed (e.g., out of process slots) */
        fprintf(stderr,
            COL_RED "sdsh: fork failed: %s\n" COL_RESET,
            strerror(errno));
        return;
    }

    if (pid == 0) {
        /* ── Child process ──────────────────────────────────────────────── */

        /*
         * Restore the default SIGINT handler so Ctrl+C properly terminates
         * child programs (e.g., a long-running grep or htop) instead of
         * being silently ignored by the custom handler we installed.
         */
        signal(SIGINT, SIG_DFL);

        execvp(argv[0], argv);

        /*
         * execvp() only returns on failure.
         * Print a user-friendly error and exit the child.
         */
        if (errno == ENOENT) {
            fprintf(stderr,
                COL_RED "sdsh: command not found: %s\n" COL_RESET,
                argv[0]);
        } else {
            fprintf(stderr,
                COL_RED "sdsh: exec failed: %s: %s\n" COL_RESET,
                argv[0], strerror(errno));
        }
        exit(EXIT_FAILURE);
    }

    /* ── Parent process: wait for child ─────────────────────────────────── */
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            /* EINTR means a signal interrupted waitpid — retry.
             * Anything else is a real error.                    */
            fprintf(stderr,
                COL_RED "sdsh: waitpid error: %s\n" COL_RESET,
                strerror(errno));
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Core dispatch: tokenize → translate → built-in or external
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_execute
 * ------------
 * Accepts a raw input line, strips it, tokenizes it, applies the
 * translation layer, then dispatches to a built-in or external runner.
 */
static void sdsh_execute(char *line)
{
    /* Trim whitespace */
    line = sdsh_trim(line);

    /* Skip blank lines and comment lines (lines starting with '#') */
    if (line[0] == '\0' || line[0] == '#')
        return;

    /* Tokenize into argv[]-style array */
    char *argv[SDSH_MAX_ARGS];
    int   argc = sdsh_tokenize(line, argv, SDSH_MAX_ARGS);

    if (argc == 0)
        return;

    /*
     * ── apt-wrapper translation ──────────────────────────────────────────
     * grab / refresh / evolve all map to the `apt` binary but require
     * an extra sub-command inserted as argv[1].  We shift existing args
     * right by one slot to make room.
     *
     *   grab <pkg>   → apt install <pkg>
     *   refresh      → apt update
     *   evolve       → apt upgrade
     */
    static const struct {
        const char *alias;
        const char *subcmd;
    } apt_table[] = {
        { "grab",    "install" },
        { "refresh", "update"  },
        { "evolve",  "upgrade" },
    };

    for (size_t i = 0; i < sizeof(apt_table) / sizeof(apt_table[0]); i++) {
        if (strcmp(argv[0], apt_table[i].alias) == 0) {
            /*
             * Shift argv[1..argc] one position to the right.
             * Guard against overflow: we need two extra slots (apt + subcmd).
             */
            if (argc + 1 >= SDSH_MAX_ARGS - 1) {
                fprintf(stderr,
                    COL_RED "sdsh: too many arguments\n" COL_RESET);
                return;
            }
            /* Move existing args right (include the current NULL sentinel) */
            for (int j = argc; j >= 1; j--)
                argv[j + 1] = argv[j];
            argv[0] = "apt";
            argv[1] = (char *)apt_table[i].subcmd;
            argc   += 1;
            break;
        }
    }

    /*
     * ── superuser translation ────────────────────────────────────────────
     * "superuser" is sdsh's alias for sudo.  We replace argv[0] in-place
     * with "sudo", then translate argv[1] (the sub-command) through the
     * apt-wrapper and 1-to-1 tables so compound forms work:
     *
     *   superuser whoami          →   sudo whoami
     *   superuser nuke /etc/x     →   sudo rm /etc/x
     *   superuser grab vim        →   sudo apt install vim
     *   superuser refresh         →   sudo apt update
     */
    if (strcmp(argv[0], "superuser") == 0) {
        if (argc < 2) {
            fprintf(stderr,
                COL_RED "sdsh: superuser: no command specified\n" COL_RESET);
            return;
        }

        /* Replace "superuser" with "sudo" in-place — no shifting needed */
        argv[0] = "sudo";

        /*
         * Now translate argv[1] (the actual sub-command).
         * First check the apt-wrapper table; if it matches, inject the
         * apt sub-command between argv[1] and argv[2].
         */
        int elevated = 1;  /* flag: we're in superuser mode */
        for (size_t i = 0; i < sizeof(apt_table) / sizeof(apt_table[0]); i++) {
            if (strcmp(argv[1], apt_table[i].alias) == 0) {
                if (argc + 1 >= SDSH_MAX_ARGS - 1) {
                    fprintf(stderr,
                        COL_RED "sdsh: too many arguments\n" COL_RESET);
                    return;
                }
                /*
                 * Shift argv[2..argc] (including NULL) right by one slot
                 * to open a gap at argv[2] for the apt sub-command.
                 */
                for (int j = argc; j >= 2; j--)
                    argv[j + 1] = argv[j];
                argv[1] = "apt";
                argv[2] = (char *)apt_table[i].subcmd;
                argc   += 1;
                elevated = 0;   /* apt already handled; skip 1-to-1 below */
                break;
            }
        }

        /* If not an apt alias, run argv[1] through the 1-to-1 table */
        if (elevated)
            argv[1] = (char *)sdsh_translate(argv[1]);

        /* Dispatch directly — skip the normal translate path below */
        if (!sdsh_run_builtin(argv, argc))
            sdsh_run_external(argv);
        return;
    }

    /* Apply the standard 1-to-1 custom-command translation to argv[0] */
    argv[0] = (char *)sdsh_translate(argv[0]);


    /* Try built-ins first; fall back to external execution */
    if (!sdsh_run_builtin(argv, argc))
        sdsh_run_external(argv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Script mode
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_run_script
 * ---------------
 * Opens a .sdsh script file and executes it line by line.
 * No interactive prompt is shown.
 */
static void sdsh_run_script(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr,
            COL_RED "sdsh: cannot open script '%s': %s\n" COL_RESET,
            path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t len;

    /* getline() allocates/grows the buffer automatically */
    while ((len = getline(&line, &cap, fp)) != -1) {
        if (g_exit_requested)
            break;

        /* Strip the trailing newline that getline leaves */
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        sdsh_execute(line);
    }

    free(line);
    fclose(fp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Interactive REPL
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * sdsh_repl
 * ---------
 * Read-Eval-Print Loop for interactive mode.
 * Runs until the user types "exit", sends EOF (Ctrl+D), or
 * g_exit_requested is set by a built-in.
 */
static void sdsh_repl(void)
{
    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t len;

    while (!g_exit_requested) {
        /* Re-draw the prompt after a Ctrl+C (SIGINT resets the line) */
        if (g_got_sigint) {
            g_got_sigint = 0;
            /* Prompt will be printed at the top of the loop; just continue */
            sdsh_print_prompt();
            continue;
        }

        sdsh_print_prompt();

        /* getline() returns -1 on EOF or read error */
        len = getline(&line, &cap, stdin);

        if (len == -1) {
            /* Check if SIGINT interrupted the read */
            if (errno == EINTR) {
                /* SIGINT handler already printed a newline;
                 * loop back to re-draw the prompt.         */
                continue;
            }
            /* True EOF (Ctrl+D) — exit cleanly */
            printf("\n");   /* tidy newline before shell exits */
            break;
        }

        /* Strip trailing newline */
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        sdsh_execute(line);
    }

    free(line);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    /* ── Install our custom SIGINT handler ──────────────────────────────── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sdsh_sigint_handler;
    sigemptyset(&sa.sa_mask);
    /*
     * SA_RESTART: automatically restart interrupted system calls
     *             (like read()) after the signal handler returns.
     *             We explicitly handle the EINTR case in getline
     *             to redraw the prompt, so this is belt-and-suspenders.
     */
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    /*
     * ── Paint the classic MS-DOS "blue screen" ───────────────────────────
     * Set background/foreground first, THEN clear — \033[2J fills the
     * screen with whatever background color is currently active, so the
     * color must be set before the clear for the whole screen to go blue.
     */
    printf(COL_BG_DOS COL_FG_DOS CLR_SCREEN);
    fflush(stdout);

    /* ── Dispatch: script mode or interactive mode ───────────────────────── */
    if (argc >= 2) {
        /* A file path was provided as the first argument → script mode */
        sdsh_run_script(argv[1]);
    } else {
        /* No argument → interactive REPL */
        sdsh_repl();
    }

    /*
     * ── Restore the terminal's normal colors before handing control back.
     * Without this, the user's shell prompt would stay tinted blue after
     * sdsh exits.
     */
    printf(COL_RESET);
    fflush(stdout);

    return EXIT_SUCCESS;
}