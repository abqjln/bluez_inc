/*
 *   Copyright (c) 2022 Martijn van Welie
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 */

#include "/mnt/code/gymlink/gl/gl_colors.h"

#include "agent.h"
#include "adapter.h"
#include "device.h"
#include "device_internal.h"
#include "logger.h"
#include <errno.h>
#include <glib.h>
#include <stdio.h>

static const char *const TAG = "Agent";

struct binc_agent {
    char *path; // Owned
    IoCapability io_capability;
    GDBusConnection *connection; // Borrowed
    guint registration_id;
    AgentRequestAuthorizationCallback request_authorization_callback;
    AgentRequestPasskeyCallback request_passkey_callback;
    AgentRequestConfirmationCallback request_confirmation_callback; // DISPLAY_YES_NO
    AgentDisplayPasskeyCallback display_passkey_callback;
};

void binc_agent_free(Agent *agent) {
    g_assert (agent != NULL);
    gboolean result = g_dbus_connection_unregister_object(agent->connection, agent->registration_id);
    if (!result) {
        log_debug(TAG, "could not unregister agent");
    }

    g_free((char *) agent->path);
    agent->path = NULL;

    agent->connection = NULL;

    g_free(agent);
}

static void bluez_agent_method_call(GDBusConnection *conn,
                                    const gchar *sender,
                                    const gchar *path,
                                    const gchar *interface,
                                    const gchar *method,
                                    GVariant *params,
                                    GDBusMethodInvocation *invocation,
                                    void *userdata) {
    guint32 pass;
    guint16 entered;
    char *object_path = NULL;
    char *pin = NULL;
    char *uuid = NULL;

    Agent *agent = (Agent *) userdata;
    g_assert(agent != NULL);

    if (g_str_equal(method, "RequestPinCode")) {
        g_variant_get(params, "(o)", &object_path);
        log_debug(TAG, "request pincode for %s", object_path);
        g_free(object_path);

        // add code to request pin
        pin = "123";
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", pin));
    } else if (g_str_equal(method, "DisplayPinCode")) {
        g_variant_get(params, "(os)", &object_path, &pin);
        log_debug(TAG, "displaying pincode %s", pin);
        g_free(object_path);
        g_free(pin);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_str_equal(method, "RequestPasskey")) {
        g_variant_get(params, "(o)", &object_path);
        Device *device = binc_agent_get_device_by_path(conn, object_path);
        g_free(object_path);

        if (device != NULL) {
            binc_device_set_bonding_state(device, BINC_BONDING);
        }

        if (agent->request_passkey_callback != NULL) {
            pass = agent->request_passkey_callback(device);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", pass));
        } else {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "No passkey inputted");
        }
    } else if (g_str_equal(method, "DisplayPasskey")) {
		// entered is always null here?
        g_variant_get(params, "(ouq)", &object_path, &pass, &entered);
        log_debug(TAG, "Enter passkey %06u on remote (%06u)", pass, entered);
        Device *device = binc_agent_get_device_by_path(conn, object_path);
        g_free(object_path);

		if (device != NULL) {
			binc_device_set_bonding_state(device, BINC_BONDING);
		}

		if (agent->display_passkey_callback != NULL) {
			agent->display_passkey_callback(device,pass);
			g_dbus_method_invocation_return_value(invocation, NULL);
		}
		else {
			log_debug(TAG, "NULL display_passkey callback (%06u)", pass);
			g_dbus_method_invocation_return_value(invocation, NULL);
		}
    } else if (g_str_equal(method, "RequestConfirmation")) {
        g_variant_get(params, "(ou)", &object_path, &pass);
        Device *device = binc_agent_get_device_by_path(conn, object_path);


log_debug(TAG,"[%s] Request confirmation '%s'[%s] '%s'",__func__,binc_device_get_name(device),binc_device_get_address(device), object_path);



        g_free(object_path);

		if (device != NULL) {

			//test
			//void gl_agent_changed_bonding_state_cb (Device *device, BondingState new_state, BondingState old_state, const GError *error); // prototype

			//binc_device_set_bonding_state_changed_cb(device,&gl_agent_changed_bonding_state_cb);

			// leave bonding state as-is
			binc_device_set_bonding_state(device, BINC_BONDING);
		}

	if (binc_device_get_bonding_state (device) != BINC_BONDED) {
		if (agent->request_confirmation_callback != NULL) {
			agent->request_confirmation_callback (device,pass);
			// If successful, cb will set BONDED, otherwise NONE
			if (binc_device_get_bonding_state (device) == BINC_BONDED) {
				g_dbus_method_invocation_return_value(invocation, NULL);
			} else {
				binc_device_set_bonding_state (device, BINC_BOND_NONE);
				g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Request confirmation failed");
			}
		}
		else {
			log_debug(TAG, "NULL request_confirmation callback--confirming passkey %06u", pass);
			g_dbus_method_invocation_return_value(invocation, NULL);
		}
	}
	else 				g_dbus_method_invocation_return_value(invocation, NULL);


// Force reconnect when changes?
//	binc_device_disconnect (device);



    } else if (g_str_equal(method, "RequestAuthorization")) {
        g_variant_get(params, "(o)", &object_path);
        log_debug(TAG, "request for authorization %s", object_path);
        Device *device = binc_agent_get_device_by_path(conn, object_path);
        g_free(object_path);
        if (device != NULL) {
            binc_device_set_bonding_state(device, BINC_BONDING);
        }
        if (agent->request_authorization_callback != NULL) {
            gboolean allowed = agent->request_authorization_callback(device);
            if (allowed) {
                g_dbus_method_invocation_return_value(invocation, NULL);
            } else {
                g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Pairing rejected");
            }
        } else {
            g_dbus_method_invocation_return_value(invocation, NULL);
        }
    } else if (g_str_equal(method, "AuthorizeService")) {
        g_variant_get(params, "(os)", &object_path, &uuid);
        log_debug(TAG, "authorize service");
        g_free(object_path);
        g_free(uuid);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_str_equal(method, "Cancel")) {
        log_debug(TAG, "cancelling pairing");
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_str_equal(method, "Release")) {
        log_debug(TAG, "agent released");
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else
        log_error(TAG, "We should not come here, unknown method");
}

static const GDBusInterfaceVTable agent_method_table = {
        .method_call = bluez_agent_method_call,
};

static int bluez_register_agent(Agent *agent) {
    GError *error = NULL;
    GDBusNodeInfo *info = NULL;

    static const gchar bluez_agent_introspection_xml[] =
            "<node name='/org/bluez/SampleAgent'>"
            "   <interface name='org.bluez.Agent1'>"
            "       <method name='Release'>"
            "       </method>"
            "       <method name='RequestPinCode'>"
            "           <arg type='o' name='device' direction='in' />"
            "           <arg type='s' name='pincode' direction='out' />"
            "       </method>"
            "       <method name='DisplayPinCode'>"
            "           <arg type='o' name='device' direction='in' />"
            "           <arg type='s' name='pincode' direction='in' />"
            "       </method>"
            "       <method name='RequestPasskey'>"
            "           <arg type='o' name='device' direction='in' />"
            "           <arg type='u' name='passkey' direction='out' />"
            "       </method>"
            "       <method name='DisplayPasskey'>"
            "           <arg type='o' name='device' direction='in' />"
            "           <arg type='u' name='passkey' direction='in' />"
            "           <arg type='q' name='entered' direction='in' />"
            "       </method>"
            "       <method name='RequestConfirmation'>"
            "           <arg type='o' name='device' direction='in' />"
            "           <arg type='u' name='passkey' direction='in' />"
            "       </method>"
            "       <method name='RequestAuthorization'>"
            "           <arg type='o' name='device' direction='in' />"
            "       </method>"
            "       <method name='AuthorizeService'>"
            "           <arg type='o' name='device' direction='in' />"
            "           <arg type='s' name='uuid' direction='in' />"
            "       </method>"
            "       <method name='Cancel'>"
            "       </method>"
            "   </interface>"
            "</node>";

    info = g_dbus_node_info_new_for_xml(bluez_agent_introspection_xml, &error);
    if (error) {
        log_error(TAG, "Unable to create node: %s\n", error->message);
        g_clear_error(&error);
        return EINVAL;
    }

    agent->registration_id = g_dbus_connection_register_object(agent->connection,
                                                               agent->path,
                                                               info->interfaces[0],
                                                               &agent_method_table,
                                                               agent, NULL, &error);
    g_dbus_node_info_unref(info);
    return 0;
}

static int binc_agentmanager_call_method(GDBusConnection *connection, const gchar *method, GVariant *param) {
    g_assert(connection != NULL);
    g_assert(method != NULL);

    GVariant *result;
    GError *error = NULL;

    result = g_dbus_connection_call_sync(connection,
                                         "org.bluez",
                                         "/org/bluez",
                                         "org.bluez.AgentManager1",
                                         method,
                                         param,
                                         NULL,
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         &error);
    g_variant_unref(result);

    if (error != NULL) {
        log_error(TAG, "AgentManager call failed '%s': %s\n", method, error->message);
        g_clear_error(&error);
        return 1;
    }

    return 0;
}

int binc_agentmanager_register_agent(Agent *agent) {
    g_assert(agent != NULL);
    char *capability = NULL;

    switch (agent->io_capability) {
        case DISPLAY_ONLY:
            capability = "DisplayOnly";
            break;
        case DISPLAY_YES_NO:
            capability = "DisplayYesNo";
            break;
        case KEYBOARD_ONLY:
            capability = "KeyboardOnly";
            break;
        case NO_INPUT_NO_OUTPUT:
            capability = "NoInputNoOutput";
            break;
        case KEYBOARD_DISPLAY:
            capability = "KeyboardDisplay";
            break;
        default:
            log_error(TAG, "[%s] Unknown IO capability specified", __func__);
            return EXIT_FAILURE;
    }
    int result = binc_agentmanager_call_method(agent->connection, "RegisterAgent",
                                               g_variant_new("(os)", agent->path, capability));
    if (result == EXIT_FAILURE) {
        log_debug(TAG, "failed to register agent");
    }

    result = binc_agentmanager_call_method(agent->connection, "RequestDefaultAgent", g_variant_new("(o)", agent->path));
    if (result == EXIT_FAILURE) {
        log_debug(TAG, "failed to register agent as default agent");
    }
    return result;
}

Agent *binc_agent_create(GDBusConnection *dbconnection, const char *path, IoCapability io_capability) {
    Agent *agent = g_new0(Agent, 1);
    agent->path = g_strdup(path);
    agent->connection = dbconnection;
    agent->io_capability = io_capability;
    bluez_register_agent(agent);
    binc_agentmanager_register_agent(agent);
    return agent;
}

void binc_agent_set_request_authorization_cb(Agent *agent, AgentRequestAuthorizationCallback callback) {
    g_assert(agent != NULL);
    g_assert(callback != NULL);
    agent->request_authorization_callback = callback;
}

void binc_agent_set_request_passkey_cb(Agent *agent, AgentRequestPasskeyCallback callback) {
    g_assert(agent != NULL);
    g_assert(callback != NULL);
    agent->request_passkey_callback = callback;
}

void binc_agent_set_request_confirmation_cb(Agent *agent, AgentRequestConfirmationCallback callback) {
    g_assert(agent != NULL);
    g_assert(callback != NULL);
    agent->request_confirmation_callback = callback;
}

void binc_agent_set_display_passkey_cb(Agent *agent, AgentDisplayPasskeyCallback callback) {
    g_assert(agent != NULL);
    g_assert(callback != NULL);
    agent->display_passkey_callback = callback;
}

const char *binc_agent_get_path(const Agent *agent) {
    g_assert(agent != NULL);
    return agent->path;
}

Device *binc_agent_get_device_by_path(GDBusConnection *dbconnection, const char *device_path) {
    LogLevel tmp = log_get_level();
    log_set_level(LOG_ERROR);
    GPtrArray *array = binc_adapter_find_all( dbconnection );
    log_set_level(tmp);
    for (guint i = 0; i < array->len; i++){
        Device *device = binc_adapter_get_device_by_path (g_ptr_array_index(array, i), device_path);
        if (device != NULL){
            g_ptr_array_free(array, TRUE);
            log_debug(TAG,CGRN"[%s]: Found '%s'[%s] at %s"CNRM,__func__,binc_device_get_name(device),binc_device_get_address(device),device_path);
            return (device);
        }
    }

    g_ptr_array_free(array, TRUE);
    return (NULL);
}

