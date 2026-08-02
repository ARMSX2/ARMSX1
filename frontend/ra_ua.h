#ifndef ARMSX_RA_UA_H
#define ARMSX_RA_UA_H

/*
    RetroAchievements client identity.

    RA identifies a client by the leading Name/Version token of the User-Agent every request
    carries. ARMSX1 identifies as ITSELF, following the same "Name/Version (Platform)" shape
    ARMSX2 uses. That is honest self-identification, and it is what gets submitted when the RA
    team registers the client.

    Being unregistered is not the same as being anonymous: until RA issues/whitelists this
    token the server simply does not treat the build as a known client. The name below is
    still the correct one to send in the meantime — it says exactly what the software is.
    If RA assigns a different token, drop it into the secret header below; nothing else
    changes.

    TO ACTIVATE A REGISTERED CLIENT
    -------------------------------
    Create frontend/ra_ua_secret.h (already in .gitignore) containing exactly:

        #define ARMSX1_RA_UA_NAME    "TheNameRAIssued"
        #define ARMSX1_RA_UA_VERSION "1.0.0"

    and rebuild. Nothing else has to change: armsx_ach_user_agent() assembles
    "<name>/<version> (Android)" from these two macros and every request picks it up.

    NEVER paste another emulator's token in here — not ARMSX2's, not anyone else's.
    Presenting a client name RA issued to a different project is impersonation; RA bans the
    account that did it, not the build.
*/

#if defined(__has_include)
#  if __has_include("ra_ua_secret.h")
#    include "ra_ua_secret.h"
#  endif
#endif

/* ARMSX1's own identity. Overridden by ra_ua_secret.h if RA issues a different token.
   Keep the version in step with android/app/build.gradle's versionName — RA uses it to tell
   builds apart in reports, so a stale number makes a fixed bug look like it is still live. */
#if !defined(ARMSX1_RA_UA_NAME)
#define ARMSX1_RA_UA_NAME "ARMSX1"
#endif

#if !defined(ARMSX1_RA_UA_VERSION)
#define ARMSX1_RA_UA_VERSION "0.1.0"
#endif

#endif
