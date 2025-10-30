#ifndef CONFIG_H
#define CONFIG_H


#include "defines.h"





#if BUILD_CONFIG == BUILD_CONFIG_PRODUCTION

    #undef USB_INIT_MAX_RETRIES
    #define USB_INIT_MAX_RETRIES                12
    
    #undef ERROR_RETRY_DELAY_MS
    #define ERROR_RETRY_DELAY_MS                1000
    
    #undef COLD_BOOT_STABILIZATION_MS
    #define COLD_BOOT_STABILIZATION_MS          3000
    
#elif BUILD_CONFIG == BUILD_CONFIG_TESTING

    #undef USB_INIT_MAX_RETRIES
    #define USB_INIT_MAX_RETRIES                3
    
    #undef ERROR_RETRY_DELAY_MS
    #define ERROR_RETRY_DELAY_MS                100
    
    #undef COLD_BOOT_STABILIZATION_MS
    #define COLD_BOOT_STABILIZATION_MS          500
    
#elif BUILD_CONFIG == BUILD_CONFIG_DEBUG

    #undef USB_INIT_MAX_RETRIES
    #define USB_INIT_MAX_RETRIES                20
    
    #undef ERROR_RETRY_DELAY_MS
    #define ERROR_RETRY_DELAY_MS                500
    
#endif

#endif // CONFIG_H

