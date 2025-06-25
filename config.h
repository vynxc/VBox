#ifndef CONFIG_H
#define CONFIG_H

// Include the consolidated defines
#include "defines.h"

//--------------------------------------------------------------------+
// Build Configuration Overrides
//--------------------------------------------------------------------+

#if BUILD_CONFIG == BUILD_CONFIG_PRODUCTION
    // Production: Conservative, reliable settings
    #undef USB_INIT_MAX_RETRIES
    #define USB_INIT_MAX_RETRIES                12
    
    #undef ERROR_RETRY_DELAY_MS
    #define ERROR_RETRY_DELAY_MS                1000
    
    #undef COLD_BOOT_STABILIZATION_MS
    #define COLD_BOOT_STABILIZATION_MS          3000
    
#elif BUILD_CONFIG == BUILD_CONFIG_TESTING
    // Testing: Fast iteration, extensive logging
    #undef USB_INIT_MAX_RETRIES
    #define USB_INIT_MAX_RETRIES                3
    
    #undef ERROR_RETRY_DELAY_MS
    #define ERROR_RETRY_DELAY_MS                100
    
    #undef COLD_BOOT_STABILIZATION_MS
    #define COLD_BOOT_STABILIZATION_MS          500
    
#elif BUILD_CONFIG == BUILD_CONFIG_DEBUG
    // Debug: Maximum retries, detailed logging
    #undef USB_INIT_MAX_RETRIES
    #define USB_INIT_MAX_RETRIES                20
    
    #undef ERROR_RETRY_DELAY_MS
    #define ERROR_RETRY_DELAY_MS                500
    
#endif

#endif // CONFIG_H

