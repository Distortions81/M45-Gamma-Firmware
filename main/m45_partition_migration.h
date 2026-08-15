#pragma once

#include "esp_err.h"

/* Returns normally only when the canonical layout is already active or migration
 * cannot be performed. A successful legacy-layout migration reboots. */
esp_err_t m45_partition_migrate_if_needed(void);
