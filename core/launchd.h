/**
 * launchd.h - Service Manager for CamelOS
 *
 * Provides a launchd-style service management system for controlling
 * the lifecycle of kernel services: registration, dependency tracking,
 * auto-start at boot, keep-alive restart on crash, and health monitoring.
 *
 * Copyright (c) 2024 CamelOS contributors
 */

#ifndef LAUNCHD_H
#define LAUNCHD_H

#include "../include/types.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

/** Maximum number of services the manager can track simultaneously */
#define LAUNCHD_MAX_SERVICES  16

/** Maximum number of dependencies per service */
#define LAUNCHD_MAX_DEPS      4

/* ------------------------------------------------------------------ */
/*  Service states                                                    */
/* ------------------------------------------------------------------ */

#define LAUNCHD_SERVICE_STOPPED   0   /* Service is not running       */
#define LAUNCHD_SERVICE_STARTING  1   /* Service is being started     */
#define LAUNCHD_SERVICE_RUNNING   2   /* Service is running normally  */
#define LAUNCHD_SERVICE_CRASHED   3   /* Service has crashed          */

/* ------------------------------------------------------------------ */
/*  Service descriptor                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char  name[32];                              /* Service identifier          */
    int   (*start_func)(void);                   /* Function pointer to start   */
    int   auto_start;                            /* Start at boot if 1          */
    int   keep_alive;                            /* Restart on crash if 1       */
    int   max_crashes;                           /* Max auto-restart attempts   */
                                                /*   (0 = unlimited)           */
    int   crash_count;                           /* Current crash count         */
    int   state;                                 /* LAUNCHD_SERVICE_*           */
    char  dependencies[LAUNCHD_MAX_DEPS][32];   /* Dependency service names    */
    int   dep_count;                             /* Number of dependencies      */
    int   active;                                /* Slot is in use (1 = yes)    */
} launchd_service_t;

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

/**
 * launchd_init - Initialize the service manager.
 *
 * Zeros the service table and prepares the subsystem for use.
 * Must be called once during boot before any other launchd call.
 */
void launchd_init(void);

/**
 * launchd_register - Register a new service with the manager.
 *
 * @name:        Null-terminated service identifier (max 31 chars)
 * @start_func:  Function pointer invoked to start the service;
 *               should return 0 on success, non-zero on failure
 * @auto_start:  If 1, the service will be started automatically at boot
 * @keep_alive:  If 1, the service will be restarted on crash
 * @max_crashes: Maximum number of automatic restart attempts;
 *               0 means unlimited retries
 *
 * Returns: slot index (>= 0) on success, -1 if no free slot
 */
int launchd_register(const char* name, int (*start_func)(void),
                     int auto_start, int keep_alive, int max_crashes);

/**
 * launchd_add_dependency - Declare that one service depends on another.
 *
 * @service_name: Name of the dependent service
 * @dep_name:     Name of the required service
 *
 * The dependent service will not start until all of its declared
 * dependencies are in the RUNNING state.
 *
 * Returns: 0 on success, -1 if service not found or dep list full
 */
int launchd_add_dependency(const char* service_name, const char* dep_name);

/**
 * launchd_boot_start - Start all services marked auto_start.
 *
 * Should be called once during boot after all services have been
 * registered and their dependencies configured.
 */
void launchd_boot_start(void);

/**
 * launchd_check_health - Check for crashed services and restart them.
 *
 * Iterates all active, keep_alive services in the CRASHED state.
 * If max_crashes is 0 (unlimited) or crash_count < max_crashes,
 * the service is restarted.  Otherwise the service is left stopped
 * and a warning is emitted.
 *
 * Should be called periodically from a timer or supervisor loop.
 */
void launchd_check_health(void);

/**
 * launchd_start_service - Start a registered service by name.
 *
 * @name: Service identifier
 *
 * If the service has dependencies, each one is verified to be RUNNING
 * first.  Dependencies that are not running will be started recursively.
 * If a dependency cannot be started, this function fails.
 *
 * Returns: 0 on success, -1 if service not found or start failed
 */
int launchd_start_service(const char* name);

/**
 * launchd_stop_service - Stop a running service by name.
 *
 * @name: Service identifier
 *
 * Sets the service state to STOPPED.  Does not invoke any stop callback.
 *
 * Returns: 0 on success, -1 if service not found
 */
int launchd_stop_service(const char* name);

/**
 * launchd_get_service_state - Query the state of a service.
 *
 * @name: Service identifier
 *
 * Returns: LAUNCHD_SERVICE_* value, or -1 if not found
 */
int launchd_get_service_state(const char* name);

/**
 * launchd_get_service_count - Count the number of registered services.
 *
 * Returns: number of active (in-use) service slots
 */
int launchd_get_service_count(void);

/**
 * launchd_get_service_info - Retrieve info about a service by slot index.
 *
 * @index:           Slot index (0 .. LAUNCHD_MAX_SERVICES-1)
 * @name_out:        Buffer to receive the service name (min 32 bytes)
 * @state_out:       Pointer to receive the service state
 * @crash_count_out: Pointer to receive the crash count
 *
 * Returns: 0 on success, -1 if index out of range or slot inactive
 */
int launchd_get_service_info(int index, char* name_out,
                             int* state_out, int* crash_count_out);

#endif /* LAUNCHD_H */
