/*
 * CoreType — shared by host, AICPU, and AICore builds.
 */
#ifndef ESL_PROXY_ONBOARD_CORE_TYPE_H
#define ESL_PROXY_ONBOARD_CORE_TYPE_H

#include <stdint.h>

#ifdef __cplusplus
#include <cstring>

enum class CoreType : int32_t {
    AIC = 0,
    AIV = 1
};

inline CoreType core_type_from_string(const char *type_str)
{
    if (type_str == nullptr) {
        return CoreType::AIC;
    }
    if (strcmp(type_str, "aic") == 0 || strcmp(type_str, "AIC") == 0) {
        return CoreType::AIC;
    }
    if (strcmp(type_str, "aiv") == 0 || strcmp(type_str, "AIV") == 0) {
        return CoreType::AIV;
    }
    return CoreType::AIC;
}

inline const char *core_type_to_string(CoreType core_type)
{
    switch (core_type) {
    case CoreType::AIC:
        return "AIC";
    case CoreType::AIV:
        return "AIV";
    default:
        return "UNKNOWN";
    }
}
#else

typedef enum CoreType {
    CORE_TYPE_AIC = 0,
    CORE_TYPE_AIV = 1
} CoreType;

#endif /* __cplusplus */

#endif /* ESL_PROXY_ONBOARD_CORE_TYPE_H */
