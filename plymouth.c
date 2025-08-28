/* plymouth.c - Plymouth plugin for the OpenRC init system
 *
 * Copyright (C) 2011  Amadeusz Żołnowski <aidecoe@gentoo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <assert.h>
#include <ctype.h>
#include <einfo.h>
#include <rc.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#include <stdbool.h>

#include "config.h"

#define PLUGIN_NAME "[plymouth-plugin]"

#ifdef DEBUG
#    define DBG(fmt, ...) einfo(PLUGIN_NAME " " fmt, ##__VA_ARGS__)
#else
#    define DBG(fmt, ...) do { (void)sizeof(fmt); } while (0)
#endif

#define PLY_INFO(fmt, ...)  einfo(PLUGIN_NAME " " fmt, ##__VA_ARGS__)
#define PLY_WARN(fmt, ...)  ewarn(PLUGIN_NAME " " fmt, ##__VA_ARGS__)
#define PLY_ERROR(fmt, ...) eerror(PLUGIN_NAME " " fmt, ##__VA_ARGS__)

#define MAX_SERVICE_NAME_LEN 64
#define RWDIR (R_OK | W_OK | X_OK)

// Command execution constants
#define MAX_ARGS 16                    // Maximum arguments for command execution
#define DAEMON_INIT_SLEEP_US 100000    // 100ms sleep for daemon initialization
#define STATUS_MSG_BUF_SIZE 160        // Buffer size for status messages
#define HOOK_STR_BUF_SIZE 16          // Buffer size for hook string conversion
#define RUN_DIR_MODE 0755             // Directory permissions for run directory

// Return codes for consistent error handling
#define PLY_SUCCESS 0                  // Success
#define PLY_ERROR_INVALID_ARG -1       // Invalid argument
#define PLY_ERROR_SPAWN_FAILED -2      // Process spawn failed
#define PLY_ERROR_DAEMON_FAILED -3     // Daemon startup failed
#define PLY_ERROR_WAIT_FAILED -4       // Process wait failed

// Plymouth binary paths
#ifndef PLYMOUTH_BINARY
#define PLYMOUTH_BINARY "/bin/plymouth"
#endif

#ifndef PLYMOUTHD_BINARY
#define PLYMOUTHD_BINARY "/sbin/plymouthd"
#endif

// Plymouth command types
typedef enum {
    PLY_CMD_PING,
    PLY_CMD_MESSAGE,
    PLY_CMD_UPDATE_STATUS,
    PLY_CMD_UPDATE_ROOTFS_RW,
    PLY_CMD_SHOW_SPLASH,
    PLY_CMD_QUIT,
    PLY_CMD_QUIT_RETAIN,
    PLY_CMD_START_DAEMON
} ply_command_t;

#ifndef RUN_DIR
#define RUN_DIR "/run/plymouth"
#endif

#ifndef PID_FILE
#define PID_FILE RUN_DIR "/pid"
#endif


enum {
    PLY_MODE_BOOT,
    PLY_MODE_SHUTDOWN
};

/**
 * Validate that required Plymouth binaries exist and are executable
 * @return true if validation passes, false otherwise
 */
static bool ply_validate_binaries(void)
{
    if (access(PLYMOUTH_BINARY, X_OK) != 0) {
        PLY_ERROR("Plymouth binary not found or not executable: %s", PLYMOUTH_BINARY);
        return false;
    }

    if (access(PLYMOUTHD_BINARY, X_OK) != 0) {
        PLY_ERROR("Plymouth daemon binary not found or not executable: %s", PLYMOUTHD_BINARY);
        return false;
    }

    DBG("Plymouth binaries validated successfully");
    return true;
}

/**
 * Execute a Plymouth command using posix_spawn
 *
 * @param cmd_type The type of command to execute
 * @param arg1 First optional argument (command-specific)
 * @param arg2 Second optional argument (command-specific)
 * @param mode Plymouth mode (PLY_MODE_BOOT or PLY_MODE_SHUTDOWN)
 * @return PLY_SUCCESS on success, or one of the PLY_ERROR_* codes on failure
 */
// execute Plymouth commands using posix_spawn
static int ply_execute_command(ply_command_t cmd_type, const char *arg1, const char *arg2, int mode)
{
    extern char **environ;
    pid_t pid;
    int status;
    char *args[MAX_ARGS];  // Max args we'll need
    int arg_count = 0;
    bool is_daemon = false;

    // Input validation
    if (cmd_type < PLY_CMD_PING || cmd_type > PLY_CMD_START_DAEMON) {
        PLY_ERROR("Invalid command type: %d", cmd_type);
        return PLY_ERROR_INVALID_ARG;
    }

    // Thread-local static buffers for command arguments - must remain valid until posix_spawn completes
    static __thread char text_buf[MAX_SERVICE_NAME_LEN * 2 + 2];
    static __thread char status_buf[MAX_SERVICE_NAME_LEN + 16];

    // Helper macro for safe argument addition with bounds checking
    #define ADD_ARG(arg) do { \
        if (arg_count >= MAX_ARGS - 1) { \
            PLY_ERROR("Too many arguments for command %d", cmd_type); \
            return PLY_ERROR_INVALID_ARG; \
        } \
        args[arg_count++] = (char*)(arg); \
    } while(0)

    const char *mode_str = (mode == PLY_MODE_BOOT) ? "boot" :
                          (mode == PLY_MODE_SHUTDOWN) ? "shutdown" : NULL;

    // Build argument array based on command type
    switch (cmd_type) {
        case PLY_CMD_PING:
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("--ping");
            break;

        case PLY_CMD_MESSAGE:
            if (!arg1 || !arg2) return PLY_ERROR_INVALID_ARG;
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("message");
            ADD_ARG("--text");
            // Combine arg1 and arg2 into single text argument
            snprintf(text_buf, sizeof(text_buf), "%s %s", arg1, arg2);
            ADD_ARG(text_buf);
            break;

        case PLY_CMD_UPDATE_STATUS:
            if (!arg1 || !arg2) return PLY_ERROR_INVALID_ARG;
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("update");
            ADD_ARG("--status");
            // Combine hook and name
            snprintf(status_buf, sizeof(status_buf), "%s-%s", arg1, arg2);
            ADD_ARG(status_buf);
            break;

        case PLY_CMD_UPDATE_ROOTFS_RW:
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("update-root-fs");
            ADD_ARG("--read-write");
            break;

        case PLY_CMD_SHOW_SPLASH:
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("--show-splash");
            break;

        case PLY_CMD_QUIT:
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("quit");
            break;

        case PLY_CMD_QUIT_RETAIN:
            ADD_ARG(PLYMOUTH_BINARY);
            ADD_ARG("quit");
            ADD_ARG("--retain-splash");
            break;

        case PLY_CMD_START_DAEMON:
            if (!mode_str) {
                PLY_ERROR("Invalid mode for daemon start: %d", mode);
                return PLY_ERROR_INVALID_ARG;
            }
            ADD_ARG(PLYMOUTHD_BINARY);
            ADD_ARG("--attach-to-session");
            ADD_ARG("--pid-file");
            ADD_ARG(PID_FILE);
            ADD_ARG("--mode");
            ADD_ARG(mode_str);
            is_daemon = true;  // Mark this as a daemon process
            break;

        default:
            PLY_ERROR("Unknown Plymouth command type: %d", cmd_type);
            return PLY_ERROR_INVALID_ARG;
    }

    // Null terminate the argument array
    args[arg_count] = NULL;

    // Log command execution details
    if (arg_count > 0) {
        DBG("Executing %s with %d args", args[0], arg_count - 1);
        #ifdef DEBUG
        for (int i = 0; i < arg_count; i++) {
            DBG("  arg[%d]: %s", i, args[i]);
        }
        #endif
    }

    int spawn_result = posix_spawnp(&pid, args[0], NULL, NULL, args, environ);
    if (spawn_result != 0) {
        PLY_ERROR("posix_spawnp(%s) failed: %s", args[0], strerror(spawn_result));
        return PLY_ERROR_SPAWN_FAILED;
    }

    // For daemon processes, don't wait - just give it a moment to start
    if (is_daemon) {
        DBG("Started daemon process %d, not waiting for exit", (int)pid);

        // Brief sleep to let daemon initialize
        usleep(DAEMON_INIT_SLEEP_US);

        // Check if daemon is still running (didn't immediately crash)
        int check_status;
        pid_t check_result = waitpid(pid, &check_status, WNOHANG);

        if (check_result == pid) {
            // Process already exited - this is an error for a daemon
            PLY_ERROR("Daemon %s exited immediately with status %d", args[0], check_status);
            return PLY_ERROR_DAEMON_FAILED;
        } else if (check_result == 0) {
            // Process still running - success for a daemon
            DBG("Daemon %s started successfully", args[0]);
            return PLY_SUCCESS;
        } else {
            // waitpid error
            PLY_ERROR("Failed to check daemon status: %s", strerror(errno));
            return PLY_ERROR_WAIT_FAILED;
        }
    }

    // For non-daemon processes, wait for completion as usual
    if (waitpid(pid, &status, 0) == -1) {
        PLY_ERROR("waitpid() failed: %s", strerror(errno));
        return PLY_ERROR_WAIT_FAILED;
    }

    return status;
}


bool ply_message(const char* hook, const char* name)
{
    int result = ply_execute_command(PLY_CMD_MESSAGE, hook, name, 0);
    return (result == PLY_SUCCESS);
}


bool ply_ping()
{
    int result = ply_execute_command(PLY_CMD_PING, NULL, NULL, 0);
    return (result == PLY_SUCCESS);
}


bool ply_quit(int mode)
{
    int rv = 0;

    if(mode == PLY_MODE_BOOT)
        rv = ply_execute_command(PLY_CMD_QUIT, NULL, NULL, 0);
    else if(mode == PLY_MODE_SHUTDOWN)
        rv = ply_execute_command(PLY_CMD_QUIT_RETAIN, NULL, NULL, 0);
    else {
        PLY_ERROR("Invalid Plymouth mode for quit: %d", mode);
        return false;
    }

    return (rv == PLY_SUCCESS);
}


bool ply_start(int mode)
{
    int rv = 0;
    const char* mode_str = (mode == PLY_MODE_BOOT) ? "boot" :
                           (mode == PLY_MODE_SHUTDOWN) ? "shutdown" : "unknown";

    if(ply_ping()) {
        DBG("Plymouth daemon already running");
        return true;
    }

    ebegin("Starting plymouthd");

    // Validate Plymouth binaries before proceeding
    if (!ply_validate_binaries()) {
        eend(1, "Plymouth binaries validation failed");
        return false;
    }

    // Ensure run directory exists with proper permissions
    if(access(RUN_DIR, RWDIR) != 0) {
        if(mkdir(RUN_DIR, RUN_DIR_MODE) != 0) {
            PLY_ERROR("Couldn't create %s: %s", RUN_DIR, strerror(errno));
            eend(1, NULL);
            return false;
        }
        DBG("Created run directory %s", RUN_DIR);
    }

    // Validate mode before proceeding
    if(mode != PLY_MODE_BOOT && mode != PLY_MODE_SHUTDOWN) {
        PLY_ERROR("Invalid Plymouth mode: %d", mode);
        eend(1, NULL);
        return false;
    }

    // Start Plymouth daemon using centralised command system
    rv = ply_execute_command(PLY_CMD_START_DAEMON, NULL, NULL, mode);

    char status_msg[STATUS_MSG_BUF_SIZE];
    status_msg[0] = '\0';
    if (rv != 0) {
        if (rv == -1) {
            snprintf(status_msg, sizeof status_msg,
                        "plymouthd(%s) failed: %s", mode_str, strerror(errno));
        } else if (WIFEXITED(rv)) {
            snprintf(status_msg, sizeof status_msg,
                        "plymouthd(%s) failed: exit=%d", mode_str, WEXITSTATUS(rv));
        } else if (WIFSIGNALED(rv)) {
            snprintf(status_msg, sizeof status_msg,
                        "plymouthd(%s) killed by signal %d%s",
                        mode_str, WTERMSIG(rv), WCOREDUMP(rv) ? " (core dumped)" : "");
        } else {
            snprintf(status_msg, sizeof status_msg,
                        "plymouthd(%s) failed: status=0x%x", mode_str, rv);
        }
    }
    eend(rv, "%s", status_msg);

    // Only show splash if daemon started successfully
    if (rv == PLY_SUCCESS) {
        int rv_splash = ply_execute_command(PLY_CMD_SHOW_SPLASH, NULL, NULL, 0);
        if (rv_splash != PLY_SUCCESS) {
            PLY_ERROR("plymouth --show-splash failed: rc=%d", rv_splash);
            // Try to clean up the daemon we just started
            ply_execute_command(PLY_CMD_QUIT, NULL, NULL, 0);
            return false;
        }
        DBG("Plymouth splash screen activated for %s mode", mode_str);
    } else {
        return false;
    }

    return true;
}


bool ply_update_status(int hook, const char* name)
{
    char hook_str[HOOK_STR_BUF_SIZE];
    snprintf(hook_str, sizeof(hook_str), "%d", hook);
    int result = ply_execute_command(PLY_CMD_UPDATE_STATUS, hook_str, name, 0);
    return (result == PLY_SUCCESS);
}


bool ply_update_rootfs_rw()
{
    const int rwdir = RWDIR;

    if(access("/var/lib/plymouth", rwdir) != 0
            || access("/var/log", rwdir) != 0) {
        PLY_ERROR("/var/lib/plymouth and /var/log need to be "
                "writable at this stage, but are not!");
        return false;
    }

    return (ply_execute_command(PLY_CMD_UPDATE_ROOTFS_RW, NULL, NULL, 0) == PLY_SUCCESS);
}


int rc_plugin_hook(RC_HOOK hook, const char *name)
{
    int rv = 0;
    char* runlevel = rc_runlevel_get();
    const char* bootlevel = getenv("RC_BOOTLEVEL");
    const char* defaultlevel = getenv("RC_DEFAULTLEVEL");

#ifdef DEBUG
    einfo("hook=%d name=%s runlvl=%s plyd=%d", hook, name, runlevel,
            ply_ping());
#endif

    /* Don't do anything if we're not booting or shutting down. */
    if(!(rc_runlevel_starting() || rc_runlevel_stopping())) {
        switch(hook) {
            case RC_HOOK_RUNLEVEL_STOP_IN:
            case RC_HOOK_RUNLEVEL_STOP_OUT:
            case RC_HOOK_RUNLEVEL_START_IN:
            case RC_HOOK_RUNLEVEL_START_OUT:
                /* Switching runlevels, so we're booting or shutting down.*/
                break;
            default:
                DBG("Not booting or shutting down");
                goto exit;
        }
    }

    DBG("switch1");

    switch(hook) {
        case RC_HOOK_RUNLEVEL_START_IN:
        case RC_HOOK_RUNLEVEL_STOP_IN:
        case RC_HOOK_SERVICE_START_IN:
        case RC_HOOK_SERVICE_STOP_IN:
            ply_update_status(hook, name);
            break;
        default:
            break;
    }

    DBG("switch2");

    switch(hook) {
    case RC_HOOK_RUNLEVEL_STOP_IN:
        /* Start the Plymouth daemon and show splash when system is being shut
         * down. */
        if(strcmp(name, RC_LEVEL_SHUTDOWN) == 0) {
            DBG("ply_start(PLY_MODE_SHUTDOWN)");
            if(!ply_start(PLY_MODE_SHUTDOWN)
                    || !ply_update_rootfs_rw())
                rv = 1;
        }
        break;

    case RC_HOOK_RUNLEVEL_START_IN:
        /* Start the Plymouth daemon and show splash when entering the boot
         * runlevel. Required /proc and /sys should already be mounted in
         * sysinit runlevel. */
        if(strcmp(name, bootlevel) == 0) {
            DBG("ply_start(PLY_MODE_BOOT)");
            if(!ply_start(PLY_MODE_BOOT))
                rv = 1;
        }
        break;

    case RC_HOOK_RUNLEVEL_START_OUT:
        /* Stop the Plymouth daemon right after default runlevel is started. */
        if(strcmp(name, defaultlevel) == 0) {
            DBG("ply_quit(PLY_MODE_BOOT)");
            if(!ply_quit(PLY_MODE_BOOT))
                rv = 1;
        }
        break;

    case RC_HOOK_SERVICE_STOP_IN:
        /* Quit Plymouth when we're going to lost write access to /var/... */
        if(strcmp(name, "mount-ro") == 0 &&
                strcmp(runlevel, RC_LEVEL_SHUTDOWN) == 0) {
            DBG("ply_quit(PLY_MODE_SHUTDOWN)");
            if(!ply_quit(PLY_MODE_SHUTDOWN))
                rv = 1;
        }
        break;

    case RC_HOOK_SERVICE_STOP_NOW:
        if(!ply_message("Stopping service", name))
            rv = 1;
        break;

    case RC_HOOK_SERVICE_START_NOW:
        if(!ply_message("Starting service", name))
            rv = 1;
        break;

    case RC_HOOK_SERVICE_START_OUT:
        /* Start Plymouth daemon if not yet started and tell we have rw access
         * to /var/... */
        if(strcmp(name, "localmount") == 0 &&
                strcmp(runlevel, RC_LEVEL_SHUTDOWN) != 0) {
            DBG("ply_update_rootfs_rw()");
            if(!ply_update_rootfs_rw())
                rv = 1;
        }
	    break;

    default:
        break;
    }

exit:
    free(runlevel);
    return rv;
}
