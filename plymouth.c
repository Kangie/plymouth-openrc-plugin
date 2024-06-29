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
#include <einfo.h>
#include <rc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"

#ifdef DEBUG
#    define DBG(x) einfo("[plymouth-plugin] " x)
#else
#    define DBG(x)
#endif

#define BUFFER_SIZE 300
#define RWDIR (R_OK | W_OK | X_OK)

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


int command(const char* cmd)
{
    int rv = system(cmd);

#ifdef DEBUG
    if(rv != 0) {
        ewarn("[plymouth-plugin] command(\"%s\"): rv=%d", cmd, rv);
    }
#endif

    return rv;
}


int commandf(const char* cmd, ...)
{
    static char buffer[BUFFER_SIZE];
    va_list ap;
    int rv;

    va_start(ap, cmd);
    rv = vsnprintf(buffer, BUFFER_SIZE, cmd, ap);
    va_end(ap);
    if(rv >= BUFFER_SIZE) {
        eerror("[plymouth-plugin] command(\"%s\"): buffer overflow", buffer);
        return -1;
    }

    return command(buffer);
}


bool ply_message(const char* hook, const char* name)
{
    return (commandf("/bin/plymouth message --text=\"%s %s\"", hook, name) == 0);
}


bool ply_ping()
{
    return (system("/bin/plymouth --ping") == 0);
}


bool ply_quit(int mode)
{
    int rv = 0;

    if(mode == PLY_MODE_BOOT)
        rv = command("/bin/plymouth quit");
    else if(mode == PLY_MODE_SHUTDOWN)
        rv = command("/bin/plymouth quit --retain-splash");
    else
        assert(0 && "Unknown mode");

    return (rv == 0);
}


bool ply_start(int mode)
{
    int rv = 0;

    if(!ply_ping()) {
        ebegin("Starting plymouthd");

        if(access(RUN_DIR, RWDIR) != 0) {
            if(mkdir(RUN_DIR, 0755) != 0) {
                eerror("[plymouth-plugin] Couldn't create " RUN_DIR);
                return false;
            }
        }

#define PLYD "/sbin/plymouthd --attach-to-session --pid-file=" PID_FILE \
    " --mode="
        if(mode == PLY_MODE_BOOT)
            rv = command(PLYD "boot");
        else if(mode == PLY_MODE_SHUTDOWN)
            rv = command(PLYD "shutdown");
        else
            assert(0 && "Unknown mode");
#undef PLYD
        eend(rv, "");

        if((rv == 0) && command("/bin/plymouth --show-splash") != 0)
            return false;
    }

    return true;
}


bool ply_update_status(int hook, const char* name)
{
    return (commandf("/bin/plymouth update --status=%d-%s", hook, name) == 0);
}


bool ply_update_rootfs_rw()
{
    const int rwdir = RWDIR;

    if(access("/var/lib/plymouth", rwdir) != 0
            || access("/var/log", rwdir) != 0) {
        eerror("[plymouth-plugin] /var/lib/plymouth and /var/log need to be "
                "writable at this stage, but are not!");
        return false;
    }

    return (command("/bin/plymouth update-root-fs --read-write") == 0);
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
