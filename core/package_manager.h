// core/package_manager.h - CamelOS Package Manager
// Manages installation, removal, and tracking of .cpkg packages
// and .dmg-based application bundles on CamelOS
#ifndef PACKAGE_MANAGER_H
#define PACKAGE_MANAGER_H

#include "../include/types.h"
#include "app_bundle.h"

// =========================================================================
// Constants
// =========================================================================

#define PKG_MAX_INSTALLED    64
#define PKG_MAX_ERROR_LEN    256
#define PKG_MAX_LINE_LEN     512
#define PKG_DB_PATH          "/Library/PackageManager/packages.db"
#define PKG_APPS_DIR         "/Applications"
#define PKG_LIB_DIR          "/Library/PackageManager"

// .cpkg section markers
#define CPKG_HEADER_START    "[CPKG]"
#define CPKG_FILES_SECTION   "[FILES]"
#define CPKG_DATA_SECTION    "[DATA]"
#define CPKG_FOOTER          "[/CPKG]"

// .cpkg metadata key names
#define CPKG_KEY_NAME        "name"
#define CPKG_KEY_VERSION     "version"
#define CPKG_KEY_IDENTIFIER  "identifier"
#define CPKG_KEY_TYPE        "type"
#define CPKG_KEY_EXECUTABLE  "executable"
#define CPKG_KEY_ICON        "icon"
#define CPKG_KEY_MIN_OS      "min_os"
#define CPKG_KEY_DESC        "description"

// Package source types
#define PKG_SOURCE_CPKG      "cpkg"
#define PKG_SOURCE_DMG       "dmg"
#define PKG_SOURCE_BUILTIN   "builtin"

// Error codes
#define PKG_OK               0
#define PKG_ERR_INVALID     -1
#define PKG_ERR_NOT_FOUND   -2
#define PKG_ERR_NO_MEM      -3
#define PKG_ERR_IO          -4
#define PKG_ERR_EXISTS      -5
#define PKG_ERR_PARSE       -6
#define PKG_ERR_FULL        -7
#define PKG_ERR_INTERNAL    -8

// =========================================================================
// Data Structures
// =========================================================================

// Installed package entry — tracks metadata beyond what AppBundleInfo holds
typedef struct {
    AppBundleInfo info;          // Parsed bundle metadata
    char install_path[256];      // Full path to .app bundle
    char install_date[32];       // When it was installed (e.g. "2025-01-01")
    char source[128];            // Where it came from (cpkg, dmg, builtin)
    int  verified;               // 1 if SHA-256 verified
} pkg_entry_t;

// Parsed .cpkg metadata (internal use during install)
typedef struct {
    char name[64];
    char version[16];
    char identifier[128];
    char type[16];
    char executable[64];
    char icon[64];
    char min_os[16];
    char description[256];
} cpkg_metadata_t;

// =========================================================================
// Public API
// =========================================================================

// Initialize the package manager subsystem
// Creates DB directory if needed, loads existing database
void pkg_init(void);

// Install a package from a .cpkg file
// Parses the archive, creates .app bundle structure, writes files,
// and registers the package in the database.
// Returns PKG_OK on success, negative error code on failure
int pkg_install(const char* cpkg_path);

// Install a package from a .dmg file
// Delegates to app_installer_quick_install and registers the result
// Returns PKG_OK on success, negative error code on failure
int pkg_install_dmg(const char* dmg_path);

// Remove an installed package by name or identifier
// Deletes the .app bundle directory recursively and removes from database
// Returns PKG_OK on success, negative error code on failure
int pkg_remove(const char* name_or_id);

// List all installed packages
// Fills out_list with AppBundleInfo structs up to max_count
// Returns the number of packages listed
int pkg_list_installed(AppBundleInfo* out_list, int max_count);

// Check for updates (placeholder — contacts update server)
// Returns number of updates available, or negative on error
int pkg_check_updates(void);

// Search available packages (placeholder — queries repository)
// Fills results buffer with newline-separated package names
// Returns number of results found, or negative on error
int pkg_search(const char* query, char* results, int max_len);

// Get package info by name or identifier
// Fills the info struct with parsed bundle metadata
// Returns PKG_OK on success, PKG_ERR_NOT_FOUND if not installed
int pkg_get_info(const char* name, AppBundleInfo* info);

// Verify a .cpkg file is valid
// Checks header, required metadata, and section structure
// Returns PKG_OK if valid, negative error code otherwise
int pkg_verify(const char* cpkg_path);

// Get last error message
// Returns a pointer to an internal static error string
const char* pkg_get_error(void);

// Get installed package count
int pkg_get_count(void);

// Rebuild the package database by scanning /Applications
// Useful if the DB is out of sync with the filesystem
// Returns number of packages found, or negative on error
int pkg_rebuild_database(void);

#endif // PACKAGE_MANAGER_H
