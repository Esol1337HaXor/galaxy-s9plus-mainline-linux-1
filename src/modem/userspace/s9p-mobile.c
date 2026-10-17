// SPDX-License-Identifier: GPL-2.0
/*
 * s9p-mobile: mobile data out of the box, doing what Android's telephony
 * does with Samsung's rild (s9p-android.service), over hwbinder:
 *
 *   1. the SIM's operator (gsm.sim.operator.numeric) and its internet APN,
 *      from Ubuntu's mobile-broadband-provider-info by MCC/MNC;
 *   2. the data slot and Samsung's mobile data switch (ISehRadio), radio on;
 *   3. the LTE attach APN, after the radio is on (libsec-ril replaces it at
 *      every power-up);
 *   4. a data registration query (libsec-ril only refreshes its PS state
 *      when asked) and the data call; rild brings the interface (rmnet0) up
 *      with the address, this adds the default route behind Wi-Fi's and the
 *      DNS servers (systemd-resolved);
 *   5. watching it, and doing 4 again when the data call is lost.
 *
 * Settings in /etc/default/s9p-modem (all optional):
 *   S9P_MOBILE_DATA=no            no mobile data
 *   S9P_APN, S9P_APN_USER, S9P_APN_PASSWORD   instead of the database's
 *   S9P_DATA_ROAMING=yes          data when roaming
 *   S9P_DATA_METRIC=750           default route metric (Wi-Fi: 600)
 * State for other programs: /run/s9p-modem/mobile (key=value).
 */
#include "s9p-ril.h"

#include <radio_client.h>
#include <radio_instance.h>
#include <radio_request.h>

#include <gbinder_reader.h>

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV		"/dev/hwbinder"
#define PROVIDERS	"/usr/share/mobile-broadband-provider-info/serviceproviders.xml"
#define STATE_FILE	"/run/s9p-modem/mobile"

static GMainLoop *loop;
static RadioInstance *radio;
static RadioClient *client;

static struct {
	char *operator;		/* MCC+MNC */
	char *provider;
	char *apn, *user, *pass;
	gboolean roaming;
	int metric;
} cfg;

static struct {
	gboolean connected;
	char *ifname;
	guint retry;		/* timeout source */
	int failures;
	char *address;
} st;

static void data_cycle(void);

static void log_msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static const char *env(const char *name, const char *def)
{
	const char *v = getenv(name);

	return v && *v ? v : def;
}

static void write_state(const char *state)
{
	char *s = g_strdup_printf("state=%s\noperator=%s\nprovider=%s\napn=%s\ninterface=%s\naddress=%s\n",
				  state, cfg.operator ? cfg.operator : "",
				  cfg.provider ? cfg.provider : "", cfg.apn ? cfg.apn : "",
				  st.ifname ? st.ifname : "", st.address ? st.address : "");

	g_mkdir_with_parents("/run/s9p-modem", 0755);
	g_file_set_contents(STATE_FILE, s, -1, NULL);
	g_free(s);
}

static void run(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void run(const char *fmt, ...)
{
	va_list ap;
	char *cmd, *out = NULL, *err = NULL;
	int status = 0;

	va_start(ap, fmt);
	cmd = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	if (!g_spawn_command_line_sync(cmd, &out, &err, &status, NULL) || status)
		log_msg("'%s' failed: %s", cmd, err ? g_strstrip(err) : "");
	g_free(cmd);
	g_free(out);
	g_free(err);
}

/* ---- the operator's APN, from mobile-broadband-provider-info ---- */

struct lookup {
	const char *mcc, *mnc;
	gboolean in_provider, in_gsm, match, in_apn, internet;
	GString *text;
	char *name, *apn, *user, *pass;
	char *cur_apn, *cur_user, *cur_pass;
	gboolean done;
};

static void lk_start(GMarkupParseContext *c, const char *el, const char **an,
		     const char **av, gpointer d, GError **e)
{
	struct lookup *l = d;
	int i;

	(void)c; (void)e;
	if (l->done)
		return;
	if (!strcmp(el, "provider")) {
		l->in_provider = TRUE;
		l->match = FALSE;
		g_clear_pointer(&l->name, g_free);
	} else if (!strcmp(el, "gsm")) {
		l->in_gsm = TRUE;
	} else if (l->in_gsm && !strcmp(el, "network-id")) {
		const char *mcc = NULL, *mnc = NULL;

		for (i = 0; an[i]; i++) {
			if (!strcmp(an[i], "mcc"))
				mcc = av[i];
			else if (!strcmp(an[i], "mnc"))
				mnc = av[i];
		}
		if (mcc && mnc && !strcmp(mcc, l->mcc) && !strcmp(mnc, l->mnc))
			l->match = TRUE;
	} else if (l->in_gsm && !strcmp(el, "apn")) {
		l->in_apn = TRUE;
		l->internet = FALSE;
		for (i = 0; an[i]; i++)
			if (!strcmp(an[i], "value"))
				l->cur_apn = g_strdup(av[i]);
	} else if (l->in_apn && !strcmp(el, "usage")) {
		for (i = 0; an[i]; i++)
			if (!strcmp(an[i], "type") && !strcmp(av[i], "internet"))
				l->internet = TRUE;
	}
	g_string_truncate(l->text, 0);
}

static void lk_end(GMarkupParseContext *c, const char *el, gpointer d, GError **e)
{
	struct lookup *l = d;

	(void)c; (void)e;
	if (l->done)
		return;
	if (!strcmp(el, "name") && l->in_provider && !l->in_gsm && !l->name) {
		l->name = g_strdup(g_strstrip(l->text->str));
	} else if (l->in_apn && !strcmp(el, "username")) {
		g_free(l->cur_user);
		l->cur_user = g_strdup(g_strstrip(l->text->str));
	} else if (l->in_apn && !strcmp(el, "password")) {
		g_free(l->cur_pass);
		l->cur_pass = g_strdup(g_strstrip(l->text->str));
	} else if (!strcmp(el, "apn")) {
		/* the matching provider's first internet APN */
		if (l->match && l->internet && !l->apn) {
			l->apn = l->cur_apn;
			l->user = l->cur_user;
			l->pass = l->cur_pass;
			l->cur_apn = l->cur_user = l->cur_pass = NULL;
			l->done = TRUE;
		}
		g_clear_pointer(&l->cur_apn, g_free);
		g_clear_pointer(&l->cur_user, g_free);
		g_clear_pointer(&l->cur_pass, g_free);
		l->in_apn = FALSE;
	} else if (!strcmp(el, "gsm")) {
		l->in_gsm = FALSE;
	} else if (!strcmp(el, "provider")) {
		l->in_provider = FALSE;
	}
}

static void lk_text(GMarkupParseContext *c, const char *t, gsize n, gpointer d, GError **e)
{
	(void)c; (void)e;
	g_string_append_len(((struct lookup *)d)->text, t, n);
}

static gboolean lookup_apn(const char *mcc, const char *mnc)
{
	static const GMarkupParser p = { lk_start, lk_end, lk_text, NULL, NULL };
	struct lookup l = { .mcc = mcc, .mnc = mnc, .text = g_string_new(NULL) };
	GMarkupParseContext *c;
	char *xml;
	gsize len;

	if (!g_file_get_contents(PROVIDERS, &xml, &len, NULL))
		return FALSE;
	c = g_markup_parse_context_new(&p, 0, &l, NULL);
	g_markup_parse_context_parse(c, xml, len, NULL);
	g_markup_parse_context_free(c);
	g_free(xml);
	g_string_free(l.text, TRUE);
	if (l.apn) {
		cfg.provider = l.name;
		cfg.apn = l.apn;
		cfg.user = l.user ? l.user : g_strdup("");
		cfg.pass = l.pass ? l.pass : g_strdup("");
		return TRUE;
	}
	g_free(l.name);
	return FALSE;
}

/* ---- the sequence ---- */

static void schedule(guint secs, GSourceFunc fn)
{
	if (st.retry)
		g_source_remove(st.retry);
	st.retry = g_timeout_add_seconds(secs, fn, NULL);
}

static gboolean data_cycle_cb(gpointer d)
{
	(void)d;
	st.retry = 0;
	data_cycle();
	return G_SOURCE_REMOVE;
}

static void configure(const RadioDataCall_1_4 *dc)
{
	const GBinderHidlString *s;
	GString *dns = g_string_new(NULL);
	const char *gw4 = NULL;
	guint i;

	g_free(st.ifname);
	st.ifname = g_strdup(dc->ifname.data.str);
	g_clear_pointer(&st.address, g_free);
	s = dc->addresses.data.ptr;
	for (i = 0; s && i < dc->addresses.count; i++)
		if (!st.address && !strchr(s[i].data.str, ':'))
			st.address = g_strdup(s[i].data.str);
	s = dc->gateways.data.ptr;
	for (i = 0; s && i < dc->gateways.count; i++)
		if (!strchr(s[i].data.str, ':') && strcmp(s[i].data.str, "0.0.0.0"))
			gw4 = s[i].data.str;
	s = dc->dnses.data.ptr;
	for (i = 0; s && i < dc->dnses.count; i++)
		if (strcmp(s[i].data.str, "0.0.0.0"))
			g_string_append_printf(dns, " %s", s[i].data.str);

	/* rild has set the address; the routes and DNS are the host's */
	run("ip link set %s up", st.ifname);
	if (gw4)
		run("ip route replace default via %s dev %s metric %d", gw4, st.ifname, cfg.metric);
	else
		run("ip route replace default dev %s metric %d", st.ifname, cfg.metric);
	run("ip -6 route replace default dev %s metric %d", st.ifname, cfg.metric);
	if (dns->len) {
		run("resolvectl dns %s%s", st.ifname, dns->str);
		run("resolvectl default-route %s true", st.ifname);
	}
	st.connected = TRUE;
	st.failures = 0;
	log_msg("mobile data up on %s: %s, DNS%s", st.ifname, st.address ? st.address : "?",
		dns->len ? dns->str : " none");
	g_string_free(dns, TRUE);
	write_state("connected");
}

static void data_done(RadioRequest *req, RADIO_TX_STATUS status, RADIO_RESP resp,
		      RADIO_ERROR err, const GBinderReader *args, gpointer d)
{
	GBinderReader r;
	const RadioDataCall_1_4 *dc = NULL;

	(void)req; (void)resp; (void)d;
	if (status == RADIO_TX_STATUS_OK && err == RADIO_ERROR_NONE) {
		gbinder_reader_copy(&r, args);
		dc = gbinder_reader_read_hidl_struct(&r, RadioDataCall_1_4);
	}
	if (dc && dc->cause == 0 && dc->active && dc->ifname.data.str && *dc->ifname.data.str) {
		configure(dc);
		return;
	}
	st.failures++;
	log_msg("data call failed (status %d, error %d, cause %d); retrying", status, err,
		dc ? dc->cause : -1);
	write_state("connecting");
	/* 20 s, then up to 2 min between attempts */
	schedule(MIN(20 * st.failures, 120), data_cycle_cb);
}

static gboolean setup_data_cb(gpointer d)
{
	GBinderWriter w;
	RadioRequest *req;

	(void)d;
	st.retry = 0;
	req = radio_request_new(client, RADIO_REQ_SETUP_DATA_CALL_1_4, &w, data_done, NULL, NULL);
	gbinder_writer_append_int32(&w, RADIO_ACCESS_NETWORK_EUTRAN);
	s9p_write_data_profile(&w, cfg.apn, cfg.user, cfg.pass, RADIO_PDP_PROTOCOL_IPV4V6);
	gbinder_writer_append_bool(&w, cfg.roaming);
	gbinder_writer_append_int32(&w, RADIO_DATA_REQUEST_REASON_NORMAL);
	gbinder_writer_append_hidl_string_vec(&w, NULL, 0);	/* addresses */
	gbinder_writer_append_hidl_string_vec(&w, NULL, 0);	/* dnses */
	radio_request_set_timeout(req, 90000);
	radio_request_submit(req);
	radio_request_unref(req);
	return G_SOURCE_REMOVE;
}

static void data_cycle(void)
{
	RadioRequest *req;

	/*
	 * libsec-ril updates its PS registration only on this query, and
	 * rejects data calls until then ("ps not regi"). Its answer does not
	 * come through libgbinder-radio; only asking matters.
	 */
	req = radio_request_new(client, RADIO_REQ_GET_DATA_REGISTRATION_STATE, NULL, NULL, NULL, NULL);
	radio_request_set_timeout(req, 5000);
	radio_request_submit(req);
	radio_request_unref(req);
	schedule(3, setup_data_cb);
}

static void attach_done(RadioRequest *req, RADIO_TX_STATUS status, RADIO_RESP resp,
			RADIO_ERROR err, const GBinderReader *args, gpointer d)
{
	(void)req; (void)resp; (void)args; (void)d;
	log_msg("attach APN %s: %s", cfg.apn, status == RADIO_TX_STATUS_OK && !err ? "set" : "failed");
	/* the CP re-attaches with it */
	schedule(15, data_cycle_cb);
}

static gboolean attach_cb(gpointer d)
{
	GBinderWriter w;
	RadioRequest *req;

	(void)d;
	st.retry = 0;
	req = radio_request_new(client, RADIO_REQ_SET_INITIAL_ATTACH_APN_1_4, &w, attach_done, NULL, NULL);
	s9p_write_data_profile(&w, cfg.apn, cfg.user, cfg.pass, RADIO_PDP_PROTOCOL_IPV4V6);
	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
	return G_SOURCE_REMOVE;
}

static void power_done(RadioRequest *req, RADIO_TX_STATUS status, RADIO_RESP resp,
		       RADIO_ERROR err, const GBinderReader *args, gpointer d)
{
	(void)req; (void)resp; (void)args; (void)d;
	log_msg("radio on: %s", status == RADIO_TX_STATUS_OK && !err ? "ok" : "failed");
	schedule(10, attach_cb);
}

static void start_radio(void)
{
	GBinderWriter w;
	RadioRequest *req;
	char *cmd;

	/* what telephony's SIM records set; libsec-ril needs it for data */
	cmd = g_strdup_printf("/usr/local/sbin/s9p-android-sh setprop gsm.sim.operator.numeric %s",
			      cfg.operator);
	run("%s", cmd);
	g_free(cmd);
	if (s9p_seh_set_data_slot(DEV, 0) || s9p_seh_mobile_data(DEV, TRUE, cfg.roaming))
		log_msg("ISehRadio not reachable: no data slot");
	req = radio_request_new(client, RADIO_REQ_SET_RADIO_POWER, &w, power_done, NULL, NULL);
	gbinder_writer_append_bool(&w, TRUE);
	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
}

static gboolean get_imsi_cb(gpointer d);

static void imsi_done(RadioRequest *req, RADIO_TX_STATUS status, RADIO_RESP resp,
		      RADIO_ERROR err, const GBinderReader *args, gpointer d)
{
	GBinderReader r;
	char *imsi = NULL, mcc[4] = "", mnc[4] = "";

	(void)req; (void)resp; (void)d;
	if (status == RADIO_TX_STATUS_OK && err == RADIO_ERROR_NONE) {
		gbinder_reader_copy(&r, args);
		imsi = gbinder_reader_read_hidl_string(&r);
	}
	if (!imsi || strlen(imsi) < 6) {
		g_free(imsi);
		log_msg("no IMSI yet (SIM not ready?)");
		write_state("no-sim");
		schedule(10, get_imsi_cb);
		return;
	}
	memcpy(mcc, imsi, 3);
	/* three-digit MNCs first (the database lists them as such), then two */
	memcpy(mnc, imsi + 3, 3);
	if (!cfg.apn && !lookup_apn(mcc, mnc)) {
		mnc[2] = 0;
		lookup_apn(mcc, mnc);
	} else if (cfg.apn) {
		mnc[2] = 0;	/* given by hand: the usual two digits */
	}
	g_free(imsi);
	cfg.operator = g_strconcat(mcc, mnc, NULL);
	if (!cfg.apn) {
		log_msg("operator %s: no APN in %s; set S9P_APN in /etc/default/s9p-modem",
			cfg.operator, PROVIDERS);
		write_state("no-apn");
		return;
	}
	log_msg("operator %s (%s), APN %s", cfg.operator, cfg.provider ? cfg.provider : "?", cfg.apn);
	write_state("connecting");
	start_radio();
}

static gboolean get_imsi_cb(gpointer d)
{
	GBinderWriter w;
	RadioRequest *req;

	(void)d;
	st.retry = 0;
	req = radio_request_new(client, RADIO_REQ_GET_IMSI_FOR_APP, &w, imsi_done, NULL, NULL);
	gbinder_writer_append_hidl_string(&w, "");
	radio_request_set_timeout(req, 30000);
	radio_request_submit(req);
	radio_request_unref(req);
	return G_SOURCE_REMOVE;
}

/* the data call is gone when its interface lost its IPv4 address */
static gboolean watch_cb(gpointer d)
{
	char *out = NULL, *cmd;

	(void)d;
	if (!st.connected || !st.ifname)
		return G_SOURCE_CONTINUE;
	cmd = g_strdup_printf("ip -4 -o addr show dev %s scope global", st.ifname);
	g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
	g_free(cmd);
	if (!out || !*out) {
		log_msg("mobile data lost on %s; reconnecting", st.ifname);
		st.connected = FALSE;
		write_state("connecting");
		schedule(5, data_cycle_cb);
	}
	g_free(out);
	return G_SOURCE_CONTINUE;
}

static void indication(RadioClient *c, RADIO_IND code, const GBinderReader *args, gpointer d)
{
	(void)c; (void)args; (void)d;
	if (code == RADIO_IND_DATA_CALL_LIST_CHANGED && st.connected)
		g_timeout_add_seconds(2, (GSourceFunc)watch_cb, NULL);
}

static gboolean connect_cb(gpointer d)
{
	(void)d;
	radio = radio_instance_new_with_version(DEV, "slot1", RADIO_INTERFACE_1_4);
	if (!radio)
		return G_SOURCE_CONTINUE;	/* rild not up yet */
	client = radio_client_new(radio);
	radio_client_add_indication_handler(client, RADIO_IND_ANY, indication, NULL);
	log_msg("connected to IRadio/slot1");
	/* give libsec-ril time to read the SIM */
	schedule(5, get_imsi_cb);
	g_timeout_add_seconds(30, watch_cb, NULL);
	return G_SOURCE_REMOVE;
}

int main(void)
{
	if (!g_ascii_strcasecmp(env("S9P_MOBILE_DATA", "yes"), "no")) {
		write_state("disabled");
		return 0;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (getenv("S9P_APN") && *getenv("S9P_APN")) {
		cfg.apn = g_strdup(getenv("S9P_APN"));
		cfg.user = g_strdup(env("S9P_APN_USER", ""));
		cfg.pass = g_strdup(env("S9P_APN_PASSWORD", ""));
		cfg.provider = g_strdup("S9P_APN");
	}
	cfg.roaming = !g_ascii_strcasecmp(env("S9P_DATA_ROAMING", "no"), "yes");
	cfg.metric = atoi(env("S9P_DATA_METRIC", "750"));
	write_state("starting");
	loop = g_main_loop_new(NULL, FALSE);
	if (connect_cb(NULL))
		g_timeout_add_seconds(5, connect_cb, NULL);
	g_main_loop_run(loop);
	return 0;
}
