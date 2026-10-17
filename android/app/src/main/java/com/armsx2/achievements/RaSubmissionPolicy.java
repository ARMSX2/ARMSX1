package com.armsx2.achievements;

/** Final transport check; login and read-only library browsing remain available while Off. */
public final class RaSubmissionPolicy {
    private RaSubmissionPolicy() {}

    public static boolean isSubmission(String data) {
        if (data == null) return false;
        int offset = 0;
        while ((offset = data.indexOf("r=", offset)) >= 0) {
            if (offset == 0 || data.charAt(offset - 1) == '&' || data.charAt(offset - 1) == '?') {
                int end = data.indexOf('&', offset + 2);
                String action = data.substring(offset + 2, end < 0 ? data.length() : end);
                return action.equals("awardachievement") || action.equals("submitlbentry") ||
                        action.equals("ping") || action.equals("startsession");
            }
            offset += 2;
        }
        return false;
    }

    public static boolean allowed(boolean enabled, String address, String post) {
        return enabled || !(isSubmission(address) || isSubmission(post));
    }
}
