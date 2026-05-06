#include "../sys/api.h"
#include "../hal/drivers/rtc.h"

// get_unix_time() and is_leap() are now implemented in hal/drivers/rtc.c
// which reads the actual RTC date and time hardware for accurate timestamps.
// This file is kept for backward compatibility.