#ifndef LOWEND_PROFILE_H
#define LOWEND_PROFILE_H

typedef enum {
    LOWEND_BALANCED = 0,
    LOWEND_MOBILE_LEGACY = 1,
    LOWEND_IGPU_LEGACY = 2
} LowendProfile;

void lowend_profile_init(void);
void lowend_profile_apply(void);
void lowend_profile_frame(float frame_ms);
LowendProfile lowend_profile_active(void);
const char *lowend_profile_name(void);
float lowend_profile_scale(void);

#endif
