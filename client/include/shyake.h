#ifndef SHYAKE_H
#define SHYAKE_H

#include <stddef.h>

/* Opaque pointer for the library context */
typedef struct shyake_ctx shyake_ctx;

/* Configuration options provided by the CLI caller */
typedef struct {
    const char *instance_url;
    const char *config_dir;
} shyake_config;

/* Initialize and free the context */
shyake_ctx* shyake_init_ctx(const shyake_config *config);
void shyake_free_ctx(shyake_ctx *ctx);

/* 
 * Core operations. 
 * Returns 0 on success, non-zero on failure.
 */

/* Generate local ML-KEM and ML-DSA keypairs */
int shyake_generate_keys(shyake_ctx *ctx);

/* Future E2EE and network operations will be added here */

#endif /* SHYAKE_H */
