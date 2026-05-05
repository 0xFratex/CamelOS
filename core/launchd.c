/**
 * launchd.c - Service Manager Implementation for CamelOS
 *
 * Implements the launchd-style service manager: registration, dependency
 * resolution, auto-start at boot, keep-alive restart on crash, and
 * periodic health monitoring.
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#include "launchd.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"

/* ------------------------------------------------------------------ */
/*  Internal state                                                    */
/* ------------------------------------------------------------------ */

/** Fixed-size service table — no dynamic allocation needed */
static launchd_service_t services[LAUNCHD_MAX_SERVICES];

/* ------------------------------------------------------------------ */
/*  Helper: find a service by name                                    */
/* ------------------------------------------------------------------ */

static int find_service(const char* name)
{
    for (int i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
        if (services[i].active && strncmp(services[i].name, name, 31) == 0) {
            return i;
        }
    }
    return -1;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void launchd_init(void)
{
    memset(services, 0, sizeof(services));
    s_printf("[launchd] Service manager initialized\n");
}

int launchd_register(const char* name, int (*start_func)(void),
                     int auto_start, int keep_alive, int max_crashes)
{
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
        if (!services[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        return -1;  /* No free slot */
    }

    /* Initialize the slot */
    strncpy(services[slot].name, name, 31);
    services[slot].name[31] = '\0';
    services[slot].start_func  = start_func;
    services[slot].auto_start  = auto_start;
    services[slot].keep_alive  = keep_alive;
    services[slot].max_crashes = max_crashes;
    services[slot].state       = LAUNCHD_SERVICE_STOPPED;
    services[slot].crash_count = 0;
    services[slot].dep_count   = 0;
    services[slot].active      = 1;

    return slot;
}

int launchd_add_dependency(const char* service_name, const char* dep_name)
{
    int idx = find_service(service_name);
    if (idx == -1) {
        return -1;
    }

    if (services[idx].dep_count >= LAUNCHD_MAX_DEPS) {
        return -1;  /* Dependency list full */
    }

    int d = services[idx].dep_count;
    strncpy(services[idx].dependencies[d], dep_name, 31);
    services[idx].dependencies[d][31] = '\0';
    services[idx].dep_count++;

    return 0;
}

void launchd_boot_start(void)
{
    for (int i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
        if (services[i].active && services[i].auto_start) {
            launchd_start_service(services[i].name);
        }
    }
    s_printf("[launchd] Boot auto-start complete\n");
}

int launchd_start_service(const char* name)
{
    int idx = find_service(name);
    if (idx == -1) {
        return -1;  /* Service not found */
    }

    /* Already running — nothing to do */
    if (services[idx].state == LAUNCHD_SERVICE_RUNNING) {
        return 0;
    }

    /* Check and start dependencies first */
    for (int d = 0; d < services[idx].dep_count; d++) {
        int dep_state = launchd_get_service_state(services[idx].dependencies[d]);

        if (dep_state != LAUNCHD_SERVICE_RUNNING) {
            /* Try to start the dependency recursively */
            int ret = launchd_start_service(services[idx].dependencies[d]);
            if (ret != 0) {
                s_printf("[launchd] Dependency %s failed for service %s\n",
                         services[idx].dependencies[d], name);
                return -1;
            }
        }
    }

    /* Attempt to start the service */
    int result = services[idx].start_func();

    if (result == 0) {
        services[idx].state       = LAUNCHD_SERVICE_RUNNING;
        services[idx].crash_count = 0;
    } else {
        services[idx].state = LAUNCHD_SERVICE_CRASHED;
        services[idx].crash_count++;
    }

    return result;
}

int launchd_stop_service(const char* name)
{
    int idx = find_service(name);
    if (idx == -1) {
        return -1;  /* Service not found */
    }

    services[idx].state = LAUNCHD_SERVICE_STOPPED;
    return 0;
}

void launchd_check_health(void)
{
    for (int i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
        if (!services[i].active) {
            continue;
        }
        if (!services[i].keep_alive) {
            continue;
        }
        if (services[i].state != LAUNCHD_SERVICE_CRASHED) {
            continue;
        }

        /* Check if restart is permitted */
        if (services[i].max_crashes == 0 ||
            services[i].crash_count < services[i].max_crashes) {
            s_printf("[launchd] Restarting crashed service: %s\n",
                     services[i].name);

            int result = services[i].start_func();
            if (result == 0) {
                services[i].state       = LAUNCHD_SERVICE_RUNNING;
            } else {
                services[i].crash_count++;
            }
        } else {
            s_printf("[launchd] Service %s exceeded max crashes (%d)\n",
                     services[i].name, services[i].crash_count);
        }
    }
}

int launchd_get_service_state(const char* name)
{
    int idx = find_service(name);
    if (idx == -1) {
        return -1;
    }
    return services[idx].state;
}

int launchd_get_service_count(void)
{
    int count = 0;
    for (int i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
        if (services[i].active) {
            count++;
        }
    }
    return count;
}

int launchd_get_service_info(int index, char* name_out,
                             int* state_out, int* crash_count_out)
{
    if (index < 0 || index >= LAUNCHD_MAX_SERVICES) {
        return -1;
    }
    if (!services[index].active) {
        return -1;
    }

    strncpy(name_out, services[index].name, 31);
    name_out[31] = '\0';
    *state_out        = services[index].state;
    *crash_count_out  = services[index].crash_count;

    return 0;
}
