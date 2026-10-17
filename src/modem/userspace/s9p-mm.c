// SPDX-License-Identifier: GPL-2.0
/*
 * ModemManager's D-Bus API for s9p-mobile (see s9p-mm.h).
 *
 * The interfaces are ModemManager's own introspection (the XML in mm/, 1.22),
 * turned into GDBus skeletons by gdbus-codegen at build time. Like
 * ModemManager, the object manager at /org/freedesktop/ModemManager1 lists
 * the modem; the SIM and the bearer are exported on their own paths.
 *
 * Every property is given a value: a container property left unset makes
 * the skeleton's GetAll fail, and libmm-glib then sees no modem at all.
 * What this modem cannot do (bands, modes, PIN changes, scans...) is left
 * unhandled, and the skeleton answers those calls with an error.
 */
#include "s9p-mm.h"
#include "s9p-mm-gen.h"
#include "s9p-mm-manager.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#define MM_NAME		"org.freedesktop.ModemManager1"
#define MM_PATH		"/org/freedesktop/ModemManager1"
#define MODEM_PATH	MM_PATH "/Modem/0"
#define SIM_PATH	MM_PATH "/SIM/0"
#define ERR_CORE	"org.freedesktop.ModemManager1.Error.Core."

/* MM enums used here */
#define CAP_GSM_UMTS		(1 << 2)
#define CAP_LTE			(1 << 3)
#define LOCK_NONE		1
#define POWER_ON		3
#define ACCESS_LTE		(1 << 14)
#define MODE_2G			(1 << 1)
#define MODE_3G			(1 << 2)
#define MODE_4G			(1 << 3)
#define IP_FAMILY_ALL		7	/* IPv4 | IPv6 | IPv4v6 */
#define PORT_NET		2
#define PORT_IGNORED		7
#define REG_IDLE		0
#define REG_HOME		1
#define BEARER_DEFAULT		1
#define IP_METHOD_STATIC	2
#define SIM_PHYSICAL		1
#define SIM_REMOVABLE		1
#define REASON_USER		1	/* MMModemStateChangeReason USER_REQUESTED */
#define REASON_UNKNOWN		0
#define FAILED_SIM_MISSING	2
#define ERR_EQUIPMENT	"org.freedesktop.ModemManager1.Error.MobileEquipment."

static GDBusConnection *bus;
static GDBusObjectManagerServer *manager;
static S9pMmModemManager1 *mm_iface;
static S9pMmObjectSkeleton *modem_obj;
static S9pMmModem *modem;
static S9pMmModemModem3gpp *m3gpp;
static S9pMmModemSimple *simple;
static S9pMmSim *sim;
static S9pMmBearer *bearer;
static char *bearer_path;
static guint bearer_count;
static char *operator_code, *operator_name;

/* a Simple.Connect or Bearer.Connect waiting for the data call */
static GDBusMethodInvocation *pending_connect;
static gboolean pending_simple;
/* Simple.Disconnect, Bearer.Disconnect or Modem.Enable(false) waiting */
static GSList *pending_disconnect;
/* Sim.SendPin or Sim.SendPuk waiting for the SIM's answer */
static GDBusMethodInvocation *pending_unlock;
static gboolean sim_missing;

static const char *const no_strings[] = { NULL };

static void export_modem(void);

static GVariant *empty_dict(void)
{
	return g_variant_new("a{sv}", NULL);
}

static GVariant *empty_au(void)
{
	return g_variant_new("au", NULL);
}

/* ---- who may ask for what ---- */

/* connecting and switching off are NetworkManager's: root only */
static gboolean caller_is_root(GDBusMethodInvocation *inv)
{
	GVariant *r;
	guint32 uid = G_MAXUINT32;

	r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
					"org.freedesktop.DBus", "GetConnectionUnixUser",
					g_variant_new("(s)", g_dbus_method_invocation_get_sender(inv)),
					G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	if (r) {
		g_variant_get(r, "(u)", &uid);
		g_variant_unref(r);
	}
	if (uid == 0)
		return TRUE;
	g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "Unauthorized",
						   "only root may change the data connection");
	return FALSE;
}

/* ---- state ---- */

static int state(void)
{
	return s9p_mm_modem_get_state(modem);
}

/* NetworkManager switched mobile broadband off */
static gboolean disabled;
/* where Enable(true) goes back to */
static int enabled_state = MM_STATE_ENABLED;

static void set_state(int new_state)
{
	int old = state();

	if (!modem || old == new_state)
		return;
	s9p_mm_modem_set_state(modem, new_state);
	s9p_mm_modem_set_access_technologies(modem, new_state >= MM_STATE_REGISTERED ? ACCESS_LTE : 0);
	s9p_mm_modem_modem3gpp_set_registration_state(m3gpp,
		new_state >= MM_STATE_REGISTERED ? REG_HOME : REG_IDLE);
	/* the new State first: NetworkManager reads it when StateChanged comes,
	 * and the skeleton would only send it on the next idle */
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(modem));
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(m3gpp));
	s9p_mm_modem_emit_state_changed(modem, old, new_state, disabled ? REASON_USER : REASON_UNKNOWN);
}

/* the radio side's progress; while disabled it only moves the way back */
void mm_set_state(int new_state)
{
	if (disabled)
		enabled_state = MIN(new_state, MM_STATE_REGISTERED);
	else
		set_state(new_state);
}

void mm_set_signal(guint quality)
{
	if (modem)
		s9p_mm_modem_set_signal_quality(modem, g_variant_new("(ub)", MIN(quality, 100), TRUE));
}

void mm_set_identity(const char *imei, const char *revision)
{
	if (!modem)
		return;
	if (imei && *imei) {
		char *id = g_compute_checksum_for_string(G_CHECKSUM_SHA1, imei, -1);

		s9p_mm_modem_set_equipment_identifier(modem, imei);
		s9p_mm_modem_set_device_identifier(modem, id);
		s9p_mm_modem_modem3gpp_set_imei(m3gpp, imei);
		g_free(id);
	}
	if (revision && *revision)
		s9p_mm_modem_set_revision(modem, revision);
}

static gboolean sim_send_pin(S9pMmSim *s, GDBusMethodInvocation *inv, const char *pin, gpointer d);
static gboolean sim_send_puk(S9pMmSim *s, GDBusMethodInvocation *inv, const char *puk,
			     const char *pin, gpointer d);

static void sim_create(void)
{
	if (!sim) {
		const char *const slots[] = { SIM_PATH, NULL };

		sim = s9p_mm_sim_skeleton_new();
		s9p_mm_sim_set_active(sim, TRUE);
		s9p_mm_sim_set_sim_identifier(sim, "");
		s9p_mm_sim_set_eid(sim, "");
		s9p_mm_sim_set_emergency_numbers(sim, no_strings);
		s9p_mm_sim_set_preferred_networks(sim, g_variant_new("a(su)", NULL));
		s9p_mm_sim_set_gid1(sim, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0, 1));
		s9p_mm_sim_set_gid2(sim, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0, 1));
		s9p_mm_sim_set_sim_type(sim, SIM_PHYSICAL);
		s9p_mm_sim_set_esim_status(sim, 0);
		s9p_mm_sim_set_removability(sim, SIM_REMOVABLE);
		s9p_mm_sim_set_imsi(sim, "");
		s9p_mm_sim_set_operator_identifier(sim, "");
		s9p_mm_sim_set_operator_name(sim, "");
		g_signal_connect(sim, "handle-send-pin", G_CALLBACK(sim_send_pin), NULL);
		g_signal_connect(sim, "handle-send-puk", G_CALLBACK(sim_send_puk), NULL);
		g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(sim), bus, SIM_PATH, NULL);
		s9p_mm_modem_set_sim(modem, SIM_PATH);
		s9p_mm_modem_set_sim_slots(modem, slots);
	}
	sim_missing = FALSE;
}

void mm_set_sim(const char *imsi, const char *code, const char *name)
{
	if (!modem)
		return;
	g_free(operator_code);
	g_free(operator_name);
	operator_code = g_strdup(code ? code : "");
	operator_name = g_strdup(name ? name : "");
	s9p_mm_modem_modem3gpp_set_operator_code(m3gpp, operator_code);
	s9p_mm_modem_modem3gpp_set_operator_name(m3gpp, operator_name);
	sim_create();
	s9p_mm_modem_set_unlock_required(modem, LOCK_NONE);
	s9p_mm_modem_set_state_failed_reason(modem, 0);
	s9p_mm_sim_set_imsi(sim, imsi ? imsi : "");
	s9p_mm_sim_set_operator_identifier(sim, operator_code);
	s9p_mm_sim_set_operator_name(sim, operator_name);
	export_modem();
}

void mm_set_sim_missing(void)
{
	if (!modem || sim_missing)
		return;
	sim_missing = TRUE;
	s9p_mm_modem_set_state_failed_reason(modem, FAILED_SIM_MISSING);
	set_state(MM_STATE_FAILED);
	export_modem();
}

static void set_retries(int lock, int retries)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{uu}"));
	if (retries >= 0)
		g_variant_builder_add(&b, "{uu}", (guint32)lock, (guint32)retries);
	s9p_mm_modem_set_unlock_retries(modem, g_variant_builder_end(&b));
}

void mm_set_locked(int lock, int retries)
{
	if (!modem)
		return;
	sim_create();
	s9p_mm_modem_set_state_failed_reason(modem, 0);
	s9p_mm_modem_set_unlock_required(modem, lock);
	set_retries(lock, retries);
	set_state(MM_STATE_LOCKED);
	export_modem();
}

void mm_unlock_done(const char *error, int retries)
{
	GDBusMethodInvocation *inv = g_steal_pointer(&pending_unlock);
	int lock;

	if (!modem)
		return;
	lock = s9p_mm_modem_get_unlock_required(modem);
	if (error) {
		set_retries(lock, retries);
		if (inv)
			g_dbus_method_invocation_return_dbus_error(inv, ERR_EQUIPMENT "IncorrectPassword", error);
		return;
	}
	s9p_mm_modem_set_unlock_required(modem, LOCK_NONE);
	set_retries(lock, -1);
	/* on to reading the SIM; NetworkManager expects this after an unlock */
	set_state(MM_STATE_INITIALIZING);
	if (!inv)
		return;
	if (lock == MM_LOCK_SIM_PUK)
		s9p_mm_sim_complete_send_puk(sim, inv);
	else
		s9p_mm_sim_complete_send_pin(sim, inv);
}

static gboolean unlock_request(GDBusMethodInvocation *inv, int wanted)
{
	if (!caller_is_root(inv))
		return FALSE;
	if (pending_unlock) {
		g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "InProgress", "already unlocking");
		return FALSE;
	}
	if (s9p_mm_modem_get_unlock_required(modem) != wanted) {
		g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "WrongState",
			wanted == MM_LOCK_SIM_PIN ? "the SIM does not want its PIN" : "the SIM does not want its PUK");
		return FALSE;
	}
	pending_unlock = inv;
	return TRUE;
}

static gboolean sim_send_pin(S9pMmSim *s, GDBusMethodInvocation *inv, const char *pin, gpointer d)
{
	(void)s; (void)d;
	if (unlock_request(inv, MM_LOCK_SIM_PIN))
		mobile_send_pin(NULL, pin);
	return TRUE;
}

static gboolean sim_send_puk(S9pMmSim *s, GDBusMethodInvocation *inv, const char *puk,
			     const char *pin, gpointer d)
{
	(void)s; (void)d;
	if (unlock_request(inv, MM_LOCK_SIM_PUK))
		mobile_send_pin(puk, pin);
	return TRUE;
}

/* ---- the bearer: one data call ---- */

static GVariant *ip_config(const struct mm_ip *ip)
{
	GVariantBuilder b;
	guint i;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	if (!ip || !ip->address)
		return g_variant_builder_end(&b);
	g_variant_builder_add(&b, "{sv}", "method", g_variant_new_uint32(IP_METHOD_STATIC));
	g_variant_builder_add(&b, "{sv}", "address", g_variant_new_string(ip->address));
	g_variant_builder_add(&b, "{sv}", "prefix", g_variant_new_uint32(ip->prefix));
	if (ip->gateway)
		g_variant_builder_add(&b, "{sv}", "gateway", g_variant_new_string(ip->gateway));
	for (i = 0; i < G_N_ELEMENTS(ip->dns) && ip->dns[i]; i++) {
		char key[8];

		g_snprintf(key, sizeof(key), "dns%u", i + 1);
		g_variant_builder_add(&b, "{sv}", key, g_variant_new_string(ip->dns[i]));
	}
	if (ip->mtu)
		g_variant_builder_add(&b, "{sv}", "mtu", g_variant_new_uint32(ip->mtu));
	return g_variant_builder_end(&b);
}

static gboolean bearer_connect(S9pMmBearer *b, GDBusMethodInvocation *inv, gpointer d);
static gboolean bearer_disconnect(S9pMmBearer *b, GDBusMethodInvocation *inv, gpointer d);

/* a new bearer for the settings NetworkManager connects with */
static void bearer_new(GVariant *props)
{
	const char *list[] = { NULL, NULL };

	if (bearer) {
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(bearer));
		g_clear_object(&bearer);
	}
	g_free(bearer_path);
	bearer_path = g_strdup_printf(MM_PATH "/Bearer/%u", bearer_count++);
	bearer = s9p_mm_bearer_skeleton_new();
	s9p_mm_bearer_set_interface(bearer, "");
	s9p_mm_bearer_set_connected(bearer, FALSE);
	s9p_mm_bearer_set_connection_error(bearer, g_variant_new("(ss)", "", ""));
	s9p_mm_bearer_set_suspended(bearer, FALSE);
	s9p_mm_bearer_set_multiplexed(bearer, FALSE);
	s9p_mm_bearer_set_ip4_config(bearer, empty_dict());
	s9p_mm_bearer_set_ip6_config(bearer, empty_dict());
	s9p_mm_bearer_set_stats(bearer, empty_dict());
	s9p_mm_bearer_set_reload_stats_supported(bearer, FALSE);
	s9p_mm_bearer_set_ip_timeout(bearer, 20);
	s9p_mm_bearer_set_bearer_type(bearer, BEARER_DEFAULT);
	s9p_mm_bearer_set_profile_id(bearer, -1);
	s9p_mm_bearer_set_properties(bearer, props ? props : empty_dict());
	g_signal_connect(bearer, "handle-connect", G_CALLBACK(bearer_connect), NULL);
	g_signal_connect(bearer, "handle-disconnect", G_CALLBACK(bearer_disconnect), NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(bearer), bus, bearer_path, NULL);
	list[0] = bearer_path;
	s9p_mm_modem_set_bearers(modem, list);
}

static void start_data_call(void)
{
	GVariant *props = s9p_mm_bearer_get_properties(bearer);
	const char *apn = "", *user = "", *pass = "";
	gboolean roaming = TRUE;

	g_variant_lookup(props, "apn", "&s", &apn);
	g_variant_lookup(props, "user", "&s", &user);
	g_variant_lookup(props, "password", "&s", &pass);
	g_variant_lookup(props, "allow-roaming", "b", &roaming);
	mm_set_state(MM_STATE_CONNECTING);
	mobile_connect(apn, user, pass, roaming);
}

void mm_connect_done(const char *ifname, const struct mm_ip *ip4, const struct mm_ip *ip6,
		     const char *error)
{
	GDBusMethodInvocation *inv = g_steal_pointer(&pending_connect);

	if (!modem)
		return;
	if (error) {
		if (bearer)
			s9p_mm_bearer_set_connection_error(bearer,
				g_variant_new("(ss)", ERR_CORE "Failed", error));
		if (state() == MM_STATE_CONNECTING)
			mm_set_state(MM_STATE_REGISTERED);
		if (inv)
			g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "Failed", error);
		return;
	}
	if (!bearer)
		bearer_new(NULL);
	s9p_mm_bearer_set_interface(bearer, ifname);
	s9p_mm_bearer_set_ip4_config(bearer, ip_config(ip4));
	s9p_mm_bearer_set_ip6_config(bearer, ip_config(ip6));
	s9p_mm_bearer_set_connection_error(bearer, g_variant_new("(ss)", "", ""));
	s9p_mm_bearer_set_connected(bearer, TRUE);
	mm_set_state(MM_STATE_CONNECTED);
	if (!inv)
		return;
	if (pending_simple)
		s9p_mm_modem_simple_complete_connect(simple, inv, bearer_path);
	else
		s9p_mm_bearer_complete_connect(bearer, inv);
}

void mm_disconnected(void)
{
	GSList *waiting = g_steal_pointer(&pending_disconnect), *l;

	if (!modem)
		return;
	if (pending_connect)
		mm_connect_done(NULL, NULL, NULL, "the data call ended");
	if (bearer) {
		s9p_mm_bearer_set_connected(bearer, FALSE);
		s9p_mm_bearer_set_ip4_config(bearer, empty_dict());
		s9p_mm_bearer_set_ip6_config(bearer, empty_dict());
	}
	if (state() > MM_STATE_REGISTERED)
		mm_set_state(MM_STATE_REGISTERED);
	for (l = waiting; l; l = l->next)
		g_dbus_method_invocation_return_value(l->data, NULL);
	g_slist_free(waiting);
}

/* end the data call, if there is one, and answer `inv` once it is gone */
static void disconnect_then_answer(GDBusMethodInvocation *inv)
{
	if (!bearer || (!s9p_mm_bearer_get_connected(bearer) && !pending_connect)) {
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	pending_disconnect = g_slist_append(pending_disconnect, inv);
	if (g_slist_length(pending_disconnect) == 1) {
		mm_set_state(MM_STATE_DISCONNECTING);
		mobile_disconnect();
	}
}

static gboolean bearer_connect(S9pMmBearer *b, GDBusMethodInvocation *inv, gpointer d)
{
	(void)b; (void)d;
	if (!caller_is_root(inv))
		return TRUE;
	if (pending_connect || state() < MM_STATE_REGISTERED) {
		g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "WrongState",
							   "not registered, or already connecting");
		return TRUE;
	}
	if (s9p_mm_bearer_get_connected(bearer)) {
		s9p_mm_bearer_complete_connect(bearer, inv);
		return TRUE;
	}
	pending_connect = inv;
	pending_simple = FALSE;
	start_data_call();
	return TRUE;
}

static gboolean bearer_disconnect(S9pMmBearer *b, GDBusMethodInvocation *inv, gpointer d)
{
	(void)b; (void)d;
	if (caller_is_root(inv))
		disconnect_then_answer(inv);
	return TRUE;
}

/* ---- Modem.Simple ---- */

static gboolean simple_connect(S9pMmModemSimple *s, GDBusMethodInvocation *inv,
			       GVariant *props, gpointer d)
{
	GVariantDict in, out;
	const char *str;
	guint32 ip_type;
	gboolean roaming;

	(void)s; (void)d;
	if (!caller_is_root(inv))
		return TRUE;
	if (pending_connect) {
		g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "InProgress",
							   "already connecting");
		return TRUE;
	}
	if (state() < MM_STATE_REGISTERED) {
		g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "WrongState",
							   state() <= MM_STATE_DISABLED ? "the modem is disabled"
							   : "not registered yet");
		return TRUE;
	}
	if (bearer && s9p_mm_bearer_get_connected(bearer)) {
		s9p_mm_modem_simple_complete_connect(simple, inv, bearer_path);
		return TRUE;
	}
	/* the bearer keeps what is about the data call: APN, login, roaming */
	g_variant_dict_init(&in, props);
	g_variant_dict_init(&out, NULL);
	if (g_variant_dict_lookup(&in, "apn", "&s", &str))
		g_variant_dict_insert(&out, "apn", "s", str);
	if (g_variant_dict_lookup(&in, "user", "&s", &str))
		g_variant_dict_insert(&out, "user", "s", str);
	if (g_variant_dict_lookup(&in, "password", "&s", &str))
		g_variant_dict_insert(&out, "password", "s", str);
	if (g_variant_dict_lookup(&in, "ip-type", "u", &ip_type))
		g_variant_dict_insert(&out, "ip-type", "u", ip_type);
	roaming = TRUE;
	g_variant_dict_lookup(&in, "allow-roaming", "b", &roaming);
	g_variant_dict_insert(&out, "allow-roaming", "b", roaming);
	g_variant_dict_clear(&in);
	bearer_new(g_variant_dict_end(&out));
	pending_connect = inv;
	pending_simple = TRUE;
	start_data_call();
	return TRUE;
}

static gboolean simple_disconnect(S9pMmModemSimple *s, GDBusMethodInvocation *inv,
				  const char *path, gpointer d)
{
	(void)s; (void)d;
	if (!caller_is_root(inv))
		return TRUE;
	if (strcmp(path, "/") && (!bearer_path || strcmp(path, bearer_path))) {
		g_dbus_method_invocation_return_dbus_error(inv, ERR_CORE "NotFound", "no such bearer");
		return TRUE;
	}
	disconnect_then_answer(inv);
	return TRUE;
}

static gboolean simple_status(S9pMmModemSimple *s, GDBusMethodInvocation *inv, gpointer d)
{
	GVariantDict st;

	(void)d;
	g_variant_dict_init(&st, NULL);
	g_variant_dict_insert(&st, "state", "u", (guint32)MAX(state(), 0));
	g_variant_dict_insert_value(&st, "signal-quality", s9p_mm_modem_get_signal_quality(modem));
	g_variant_dict_insert(&st, "access-technologies", "u", s9p_mm_modem_get_access_technologies(modem));
	g_variant_dict_insert(&st, "m3gpp-registration-state", "u",
			      s9p_mm_modem_modem3gpp_get_registration_state(m3gpp));
	g_variant_dict_insert(&st, "m3gpp-operator-code", "s", operator_code ? operator_code : "");
	g_variant_dict_insert(&st, "m3gpp-operator-name", "s", operator_name ? operator_name : "");
	s9p_mm_modem_simple_complete_get_status(s, inv, g_variant_dict_end(&st));
	return TRUE;
}

/* ---- Modem ---- */

static gboolean modem_enable(S9pMmModem *m, GDBusMethodInvocation *inv, gboolean on, gpointer d)
{
	(void)d;
	if (!caller_is_root(inv))
		return TRUE;
	if (on) {
		if (disabled) {
			disabled = FALSE;
			set_state(enabled_state);
			mobile_enable(TRUE);
		}
		s9p_mm_modem_complete_enable(m, inv);
		return TRUE;
	}
	if (disabled) {
		s9p_mm_modem_complete_enable(m, inv);
		return TRUE;
	}
	/* off: no data; the radio stays on (calls, and a fast way back) */
	enabled_state = MIN(state(), MM_STATE_REGISTERED);
	set_state(MM_STATE_DISABLED);
	disabled = TRUE;
	mobile_enable(FALSE);
	if (bearer && (s9p_mm_bearer_get_connected(bearer) || pending_connect)) {
		pending_disconnect = g_slist_append(pending_disconnect, inv);
		mobile_disconnect();
		/* answered from mm_disconnected() */
	} else {
		s9p_mm_modem_complete_enable(m, inv);
	}
	return TRUE;
}

static gboolean modem_power(S9pMmModem *m, GDBusMethodInvocation *inv, guint power, gpointer d)
{
	(void)d;
	if (!caller_is_root(inv))
		return TRUE;
	/* NetworkManager asks for low power after disabling: noted, nothing more */
	s9p_mm_modem_set_power_state(m, power);
	s9p_mm_modem_complete_set_power_state(m, inv);
	return TRUE;
}

static gboolean modem_list_bearers(S9pMmModem *m, GDBusMethodInvocation *inv, gpointer d)
{
	const char *list[] = { bearer_path, NULL };

	(void)d;
	s9p_mm_modem_complete_list_bearers(m, inv, list);
	return TRUE;
}

/* ---- setup ---- */

static void modem_init(void)
{
	GVariantBuilder b;
	const char *const drivers[] = { "s9p-modem", NULL };
	guint32 caps = CAP_GSM_UMTS | CAP_LTE;

	modem = s9p_mm_modem_skeleton_new();
	s9p_mm_modem_set_sim(modem, "/");
	s9p_mm_modem_set_sim_slots(modem, no_strings);
	s9p_mm_modem_set_primary_sim_slot(modem, 0);
	s9p_mm_modem_set_bearers(modem, no_strings);
	s9p_mm_modem_set_supported_capabilities(modem, g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32,
										&caps, 1, sizeof(caps)));
	s9p_mm_modem_set_current_capabilities(modem, caps);
	s9p_mm_modem_set_max_bearers(modem, 1);
	s9p_mm_modem_set_max_active_bearers(modem, 1);
	s9p_mm_modem_set_max_active_multiplexed_bearers(modem, 0);
	s9p_mm_modem_set_manufacturer(modem, "Samsung");
	s9p_mm_modem_set_model(modem, "Galaxy S9+ (Shannon 360)");
	s9p_mm_modem_set_revision(modem, "");
	s9p_mm_modem_set_carrier_configuration(modem, "");
	s9p_mm_modem_set_carrier_configuration_revision(modem, "");
	s9p_mm_modem_set_hardware_revision(modem, "");
	s9p_mm_modem_set_device_identifier(modem, "s9p-modem");
	s9p_mm_modem_set_device(modem, "/sys/devices/platform/s9p-modem");
	s9p_mm_modem_set_physdev(modem, "/sys/devices/platform/s9p-modem");
	s9p_mm_modem_set_drivers(modem, drivers);
	s9p_mm_modem_set_plugin(modem, "s9p-mobile");
	/* NetworkManager names the device after the primary port: not a netdev */
	s9p_mm_modem_set_primary_port(modem, "umts_ipc0");
	g_variant_builder_init(&b, G_VARIANT_TYPE("a(su)"));
	g_variant_builder_add(&b, "(su)", "umts_ipc0", PORT_IGNORED);
	g_variant_builder_add(&b, "(su)", "rmnet0", PORT_NET);
	s9p_mm_modem_set_ports(modem, g_variant_builder_end(&b));
	s9p_mm_modem_set_equipment_identifier(modem, "");
	s9p_mm_modem_set_unlock_required(modem, LOCK_NONE);
	s9p_mm_modem_set_unlock_retries(modem, g_variant_new("a{uu}", NULL));
	s9p_mm_modem_set_state(modem, MM_STATE_INITIALIZING);
	s9p_mm_modem_set_state_failed_reason(modem, 0);
	s9p_mm_modem_set_access_technologies(modem, 0);
	s9p_mm_modem_set_signal_quality(modem, g_variant_new("(ub)", 0, FALSE));
	s9p_mm_modem_set_own_numbers(modem, no_strings);
	s9p_mm_modem_set_power_state(modem, POWER_ON);
	g_variant_builder_init(&b, G_VARIANT_TYPE("a(uu)"));
	g_variant_builder_add(&b, "(uu)", MODE_2G | MODE_3G | MODE_4G, MODE_4G);
	s9p_mm_modem_set_supported_modes(modem, g_variant_builder_end(&b));
	s9p_mm_modem_set_current_modes(modem, g_variant_new("(uu)", MODE_2G | MODE_3G | MODE_4G, MODE_4G));
	s9p_mm_modem_set_supported_bands(modem, empty_au());
	s9p_mm_modem_set_current_bands(modem, empty_au());
	s9p_mm_modem_set_supported_ip_families(modem, IP_FAMILY_ALL);
	g_signal_connect(modem, "handle-enable", G_CALLBACK(modem_enable), NULL);
	g_signal_connect(modem, "handle-set-power-state", G_CALLBACK(modem_power), NULL);
	g_signal_connect(modem, "handle-list-bearers", G_CALLBACK(modem_list_bearers), NULL);

	m3gpp = s9p_mm_modem_modem3gpp_skeleton_new();
	s9p_mm_modem_modem3gpp_set_imei(m3gpp, "");
	s9p_mm_modem_modem3gpp_set_registration_state(m3gpp, REG_IDLE);
	s9p_mm_modem_modem3gpp_set_operator_code(m3gpp, "");
	s9p_mm_modem_modem3gpp_set_operator_name(m3gpp, "");
	s9p_mm_modem_modem3gpp_set_enabled_facility_locks(m3gpp, 0);
	s9p_mm_modem_modem3gpp_set_subscription_state(m3gpp, 0);
	s9p_mm_modem_modem3gpp_set_eps_ue_mode_operation(m3gpp, 0);
	s9p_mm_modem_modem3gpp_set_pco(m3gpp, g_variant_new("a(ubay)", NULL));
	s9p_mm_modem_modem3gpp_set_initial_eps_bearer(m3gpp, "/");
	s9p_mm_modem_modem3gpp_set_initial_eps_bearer_settings(m3gpp, empty_dict());
	s9p_mm_modem_modem3gpp_set_packet_service_state(m3gpp, 0);
	s9p_mm_modem_modem3gpp_set_nr5g_registration_settings(m3gpp, empty_dict());

	simple = s9p_mm_modem_simple_skeleton_new();
	g_signal_connect(simple, "handle-connect", G_CALLBACK(simple_connect), NULL);
	g_signal_connect(simple, "handle-disconnect", G_CALLBACK(simple_disconnect), NULL);
	g_signal_connect(simple, "handle-get-status", G_CALLBACK(simple_status), NULL);

	modem_obj = s9p_mm_object_skeleton_new(MODEM_PATH);
	s9p_mm_object_skeleton_set_modem(modem_obj, modem);
	s9p_mm_object_skeleton_set_modem_modem3gpp(modem_obj, m3gpp);
	s9p_mm_object_skeleton_set_modem_simple(modem_obj, simple);
}

static gboolean name_owned, modem_exported;

/* on the bus once the name is ours and the SIM is known (or known missing) */
static void export_modem(void)
{
	if (modem_exported || !name_owned || (!sim && !sim_missing))
		return;
	modem_exported = TRUE;
	g_dbus_object_manager_server_export(manager, G_DBUS_OBJECT_SKELETON(modem_obj));
}

static gboolean export_modem_cb(gpointer d)
{
	(void)d;
	name_owned = TRUE;
	export_modem();
	return G_SOURCE_REMOVE;
}

/*
 * The modem appears a moment after the name, as with ModemManager, which
 * probes its devices first. When the name gets an owner, NetworkManager
 * drops its object manager client and makes a new one; a modem already
 * there reaches it through the old client, and NetworkManager keeps that
 * one, whose proxies then never hear a change ("already exists, ignoring").
 * And NetworkManager asks for the SIM once, when the modem appears: one that
 * comes later is never seen, and a connection waits for it forever.
 */
static void name_acquired(GDBusConnection *c, const char *name, gpointer d)
{
	(void)c; (void)name; (void)d;
	g_timeout_add_seconds(2, export_modem_cb, NULL);
}

static void name_lost(GDBusConnection *c, const char *name, gpointer d)
{
	(void)d;
	/* another ModemManager has the name: stay off the bus, data still works */
	if (manager && c)
		g_dbus_object_manager_server_set_connection(manager, NULL);
	printf("could not own %s (ModemManager running?): no desktop integration\n", name);
	fflush(stdout);
}

void mm_start(void)
{
	GError *err = NULL;

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		printf("no system bus: %s\n", err->message);
		g_error_free(err);
		return;
	}
	modem_init();
	mm_iface = s9p_mm_modem_manager1_skeleton_new();
	s9p_mm_modem_manager1_set_version(mm_iface, "1.22.0-s9p");
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(mm_iface), bus, MM_PATH, NULL);
	manager = g_dbus_object_manager_server_new(MM_PATH);
	/* registered, empty, before the name is asked for: clients call
	 * GetManagedObjects as soon as the name has an owner */
	g_dbus_object_manager_server_set_connection(manager, bus);
	g_bus_own_name_on_connection(bus, MM_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
				     name_acquired, name_lost, NULL, NULL);
}
