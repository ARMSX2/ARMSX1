package com.armsx2

import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** Fails if local tests accidentally resolve android.jar's throwing/no-op JSON stubs. */
class HostJsonRuntimeTest {
    @Test fun jsonValuesSurviveASerializationRoundTrip() {
        val original = JSONObject()
            .put("loggedIn", true)
            .put("userName", "Test User")
            .put("missing", JSONObject.NULL)
            .put("items", JSONArray().put(JSONObject().put("id", 42)))

        val restored = JSONObject(original.toString())
        assertTrue(restored.getBoolean("loggedIn"))
        assertEquals("Test User", restored.getString("userName"))
        assertTrue(restored.isNull("missing"))
        assertFalse(restored.has("absent"))
        assertEquals(1, restored.getJSONArray("items").length())
        assertEquals(42, restored.getJSONArray("items").getJSONObject(0).getInt("id"))
    }

    @Test fun invalidJsonIsRejectedInsteadOfReturningDefaults() {
        assertThrows(JSONException::class.java) { JSONObject("<html>offline</html>") }
        assertThrows(JSONException::class.java) { JSONArray("not an array") }
    }
}
