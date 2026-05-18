#ifndef SHYAKE_INTERNAL_H
#define SHYAKE_INTERNAL_H

#include "shyake.h"

/* Internal context definition hidden from the public ABI */
struct shyake_ctx {
    char *instance_url;
    char *config_dir;
    
    /* 
     * In the future, long-lived resources like CURLM (connection pool) 
     * or cached keys can be stored here.
     */
};

#endif /* SHYAKE_INTERNAL_H */
