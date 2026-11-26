#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// RecompAchievements channel client (RL_SESSION pipe/socket).
// Safe no-op when RL_SESSION is not set.
void achievements_tick(void);
void achievements_notify_casual(const char *reason);
int achievements_connected(void);
int achievements_is_casual(void);

#ifdef __cplusplus
}
#endif
