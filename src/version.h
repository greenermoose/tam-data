#ifndef TD_VERSION_H
#define TD_VERSION_H

#define TD_VERSION_MAJOR 1
#define TD_VERSION_MINOR 0
#define TD_VERSION_PATCH 0
#define TD_VERSION_STRING "1.0.0"

#ifndef TD_BUILD_ID
#define TD_BUILD_ID ""
#endif

static inline const char *td_version(void)
{
    return TD_VERSION_STRING;
}

static inline const char *td_build_id(void)
{
    return TD_BUILD_ID;
}

#endif /* TD_VERSION_H */
