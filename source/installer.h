#pragma once

#include <stdatomic.h>
#include "catalog.h"
#include "download.h"

typedef enum {
    READY, CHECKING, PREPARING, DOWNLOADING, VERIFYING, SAVING,
    INSTALLED, ALREADY_INSTALLED, CONFLICT, CANCELLED, FAILED
} InstallPhase;

typedef struct {
    atomic_int phase;
    atomic_uint downloaded, total;
    atomic_bool cancel;
    const StorePackage *package;
    StoreInstallError error;
} InstallJob;

void install_package(void *argument);
