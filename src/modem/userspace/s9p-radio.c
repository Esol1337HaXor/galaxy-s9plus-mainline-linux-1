// SPDX-License-Identifier: GPL-2.0
/*
 * s9p-radio: a small IRadio client for testing the modem, talking to
 * Samsung's rild (in the Android container of extras/modem) over hwbinder
 * with libgbinder-radio.
 *
 *   s9p-radio [-d /dev/hwbinder] status        baseband, SIM, registration
 *   s9p-radio on | off                         radio power
 *   s9p-radio pin 1234                         unlock the SIM
 *   s9p-radio dial +34600000000 [seconds]      call, hang up after the time
 *   s9p-radio hangup
 *   s9p-radio attach APN [user pass]           APN for the LTE attach
 *   s9p-radio data APN [user pass]             set up a data call
 *   s9p-radio mobiledata [on|off]              Samsung's mobile data switch
 *   s9p-radio dds [slot]                       this slot carries mobile data (Samsung)
 *   s9p-radio ids                              IMEI and the start of the IMSI
 *   s9p-radio mode 9                         preferred network (0 2G/3G, 1 2G, 9 LTE/3G/2G)
 *   s9p-radio watch [seconds]                  print indications
 */
#include <radio_client.h>
#include <radio_instance.h>
#include <radio_request.h>
#include <radio_util.h>

#include <gbinder_buffer.h>
#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *loop;
static RadioClient *client;
static int exit_code;

static const char *card_state[] = { "absent", "present", "error", "restricted" };
static const char *app_state[] = { "unknown", "detected", "pin", "puk",
				   "subscription-perso", "ready" };
static const char *reg_state[] = { "not-registered", "home", "searching",
				   "denied", "unknown", "roaming" };

#define NAME(tab, i) ((i) >= 0 && (guint)(i) < G_N_ELEMENTS(tab) ? tab[i] : "?")

static gboolean quit_cb(gpointer data)
{
	(void)data;
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static gboolean check(RADIO_TX_STATUS status, RADIO_ERROR error, const char *what)
{
	if (status != RADIO_TX_STATUS_OK) {
		printf("%s: transaction failed (%d)\n", what, status);
		exit_code = 1;
		return FALSE;
	}
	if (error != RADIO_ERROR_NONE) {
		printf("%s: error %d\n", what, error);
		exit_code = 1;
		return FALSE;
	}
	return TRUE;
}

/* a struct sent by value: its buffer object, whatever its version */
static const gint32 *read_struct(GBinderReader *r, gsize *size)
{
	GBinderBuffer *buf = gbinder_reader_read_buffer(r);

	if (!buf)
		return NULL;
	*size = buf->size;
	return buf->data;	/* kept until exit: the tool is short-lived */
}

/* ---- status: one request after the other ---- */

static void status_next(int step);

static void baseband_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
			  RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	char *v;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "baseband")) {
		gbinder_reader_copy(&r, args);
		v = gbinder_reader_read_hidl_string(&r);
		printf("baseband: %s\n", v ? v : "?");
		g_free(v);
	}
	status_next(1);
}

static void icc_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		     RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	const RadioCardStatus *cs;
	gsize size;
	guint i;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "sim")) {
		gbinder_reader_copy(&r, args);
		cs = (const RadioCardStatus *)read_struct(&r, &size);
		if (cs && size >= sizeof(*cs)) {
			const RadioAppStatus *apps = cs->apps.data.ptr;

			printf("sim: card %s, %u app(s)\n", NAME(card_state, cs->cardState),
			       cs->apps.count);
			/* AppStatus_1_5 is bigger; 1.4 replies carry the 1.0 one */
			for (i = 0; apps && i < cs->apps.count; i++)
				printf("sim: app %u type %d state %s pin1 %d\n", i,
				       apps[i].appType, NAME(app_state, apps[i].appState),
				       apps[i].pin1);
		}
	}
	status_next(2);
}

static void voice_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		       RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	const gint32 *v;
	gsize size;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "voice registration")) {
		gbinder_reader_copy(&r, args);
		v = read_struct(&r, &size);
		if (v && size >= 8)
			printf("voice: %s, radio technology %d\n", NAME(reg_state, v[0] % 10), v[1]);
	}
	status_next(3);
}

static void operator_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
			  RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	char *l, *s, *n;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "operator")) {
		gbinder_reader_copy(&r, args);
		l = gbinder_reader_read_hidl_string(&r);
		s = gbinder_reader_read_hidl_string(&r);
		n = gbinder_reader_read_hidl_string(&r);
		printf("operator: %s / %s / %s\n", l ? l : "", s ? s : "", n ? n : "");
		g_free(l); g_free(s); g_free(n);
	}
	status_next(4);
}

static void signal_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
			RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	const gint32 *v;
	gsize size, i;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "signal")) {
		gbinder_reader_copy(&r, args);
		v = read_struct(&r, &size);
		printf("signal:");
		for (i = 0; v && i < size / 4; i++)
			printf(" %d", v[i]);
		printf("\n");
	}
	status_next(5);
}

static void submit(RADIO_REQ code, RadioRequestCompleteFunc done)
{
	RadioRequest *req = radio_request_new(client, code, NULL, done, NULL, NULL);

	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
}

static void status_next(int step)
{
	switch (step) {
	case 0: submit(RADIO_REQ_GET_BASEBAND_VERSION, baseband_done); break;
	case 1: submit(RADIO_REQ_GET_ICC_CARD_STATUS, icc_done); break;
	case 2: submit(RADIO_REQ_GET_VOICE_REGISTRATION_STATE, voice_done); break;
	case 3: submit(RADIO_REQ_GET_OPERATOR, operator_done); break;
	case 4: submit(RADIO_REQ_GET_SIGNAL_STRENGTH, signal_done); break;
	default: g_main_loop_quit(loop);
	}
}

/* ---- identities ---- */

static void ids_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		     RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	char *v;
	int i;

	(void)req; (void)resp;
	if (check(st, err, data)) {
		gbinder_reader_copy(&r, args);
		/* getDeviceIdentity: imei, imeisv, esn, meid; getIMSIForApp: imsi */
		for (i = 0; i < 4 && (v = gbinder_reader_read_hidl_string(&r)); i++) {
			/* only the first digits of the IMSI: it identifies the subscriber */
			if (!strcmp(data, "imsi") && strlen(v) > 5)
				v[5] = 0;
			printf("%s[%d]: %s\n", (const char *)data, i, v);
			g_free(v);
		}
	}
	if (!strcmp(data, "imsi"))
		g_main_loop_quit(loop);
}

static void ids(void)
{
	GBinderWriter w;
	RadioRequest *req = radio_request_new(client, RADIO_REQ_GET_DEVICE_IDENTITY, NULL,
					      ids_done, NULL, "identity");

	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
	req = radio_request_new(client, RADIO_REQ_GET_IMSI_FOR_APP, &w, ids_done, NULL, "imsi");
	gbinder_writer_append_hidl_string(&w, "");
	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
}

/* ---- mobile data ---- */

static void generic_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
			 RADIO_ERROR err, const GBinderReader *args, gpointer data);

/* a string field of a struct already written as buffer object idx */
static void put_string(GBinderWriter *w, guint idx, GBinderHidlString *f,
		       gsize offset, const char *str)
{
	GBinderParent parent = { .index = idx, .offset = offset };

	gbinder_writer_append_buffer_object_with_parent(w, f->data.str, f->len + 1, &parent);
}

static void set_string(GBinderWriter *w, GBinderHidlString *f, const char *str)
{
	f->data.str = gbinder_writer_strdup(w, str);
	f->len = strlen(str);
	f->owns_buffer = TRUE;
}

static void print_strings(const char *what, const GBinderHidlVec *v)
{
	const GBinderHidlString *s = v->data.ptr;
	guint i;

	for (i = 0; s && i < v->count; i++)
		printf("data: %s %s\n", what, s[i].data.str);
}

static void data_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		      RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	const RadioDataCall_1_4 *dc;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "data call")) {
		gbinder_reader_copy(&r, args);
		dc = gbinder_reader_read_hidl_struct(&r, RadioDataCall_1_4);
		if (dc) {
			printf("data: cause %d cid %d active %d type %d mtu %d ifname %s\n",
			       dc->cause, dc->cid, dc->active, dc->type, dc->mtu,
			       dc->ifname.data.str ? dc->ifname.data.str : "?");
			print_strings("address", &dc->addresses);
			print_strings("dns", &dc->dnses);
			print_strings("gateway", &dc->gateways);
		}
	}
	g_main_loop_quit(loop);
}

/* S9P_PDP=ip|ipv6|ipv4v6 (default ipv4v6) */
static RADIO_PDP_PROTOCOL_TYPE pdp_protocol(void)
{
	const char *p = getenv("S9P_PDP");

	if (p && !strcmp(p, "ip"))
		return RADIO_PDP_PROTOCOL_IP;
	if (p && !strcmp(p, "ipv6"))
		return RADIO_PDP_PROTOCOL_IPV6;
	return RADIO_PDP_PROTOCOL_IPV4V6;
}

static void write_profile(GBinderWriter *w, const char *apn, const char *user,
			  const char *pass)
{
	RadioDataProfile_1_4 *dp = gbinder_writer_new0(w, RadioDataProfile_1_4);
	guint idx;

	dp->profileId = RADIO_DATA_PROFILE_DEFAULT;
	set_string(w, &dp->apn, apn);
	dp->protocol = dp->roamingProtocol = pdp_protocol();
	dp->authType = *user ? RADIO_APN_AUTH_PAP_CHAP : RADIO_APN_AUTH_NONE;
	set_string(w, &dp->user, user);
	set_string(w, &dp->password, pass);
	dp->type = 1;	/* 3GPP */
	dp->enabled = TRUE;
	dp->supportedApnTypesBitmap = RADIO_APN_TYPE_DEFAULT;
	dp->preferred = dp->persistent = TRUE;
	idx = gbinder_writer_append_buffer_object(w, dp, sizeof(*dp));
	put_string(w, idx, &dp->apn, G_STRUCT_OFFSET(RadioDataProfile_1_4, apn.data.str), apn);
	put_string(w, idx, &dp->user, G_STRUCT_OFFSET(RadioDataProfile_1_4, user.data.str), user);
	put_string(w, idx, &dp->password, G_STRUCT_OFFSET(RadioDataProfile_1_4, password.data.str), pass);
}

static void attach_apn(const char *apn, const char *user, const char *pass)
{
	GBinderWriter w;
	RadioRequest *req = radio_request_new(client, RADIO_REQ_SET_INITIAL_ATTACH_APN_1_4, &w,
					      generic_done, NULL, "attach apn");

	write_profile(&w, apn, user, pass);
	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
}

static void setup_data(const char *apn, const char *user, const char *pass)
{
	GBinderWriter w;
	RadioRequest *req;

	/*
	 * No IRadio setDataAllowed here: it reaches libsec-ril without a phone
	 * id and undoes "s9p-radio dds", which has to come first.
	 */
	req = radio_request_new(client, RADIO_REQ_SETUP_DATA_CALL_1_4, &w, data_done, NULL, NULL);
	gbinder_writer_append_int32(&w, RADIO_ACCESS_NETWORK_EUTRAN);
	write_profile(&w, apn, user, pass);
	gbinder_writer_append_bool(&w, FALSE);			/* roamingAllowed */
	gbinder_writer_append_int32(&w, RADIO_DATA_REQUEST_REASON_NORMAL);
	gbinder_writer_append_hidl_string_vec(&w, NULL, 0);	/* addresses */
	gbinder_writer_append_hidl_string_vec(&w, NULL, 0);	/* dnses */
	radio_request_set_timeout(req, 60000);
	radio_request_submit(req);
	radio_request_unref(req);
}

/* ---- data subscription (Samsung's ISehRadio) ---- */

#include <gbinder_client.h>
#include <gbinder_local_request.h>
#include <gbinder_servicemanager.h>

/*
 * ISehRadio@2.0::setDataAllowed(serial, allow, SehAllowDataParam {int32
 * defaultDataPhoneId}), transaction 38 (from the HIDL proxy in
 * vendor.samsung.hardware.radio@2.0.so). It reaches libsec-ril as
 * SEC_ALLOW_DATA with the phone id, which IRadio's setDataAllowed leaves out
 * (-1): libsec-ril then never takes this slot as the data one, ignores its PS
 * registration and rejects every data call. It is oneway; the answer goes to
 * ISehRadioResponse, which nobody registers here.
 */
#define SEH_RADIO_FQNAME	"vendor.samsung.hardware.radio@2.1::ISehRadio/slot1"
#define SEH_RADIO_2_0		"vendor.samsung.hardware.radio@2.0::ISehRadio"
#define SEH_TX_SET_DATA_ALLOWED	38
/* setMobileDataSetting(serial, bool enabled, bool roaming): the data switch */
#define SEH_TX_SET_MOBILE_DATA	39

static int seh_transact(const char *dev, guint32 code,
			void (*args)(GBinderWriter *w, gpointer data), gpointer data)
{
	GBinderServiceManager *sm = gbinder_servicemanager_new(dev);
	GBinderRemoteObject *obj;
	GBinderClient *c;
	GBinderLocalRequest *req;
	GBinderWriter w;
	int status = 0, ret;

	obj = sm ? gbinder_servicemanager_get_service_sync(sm, SEH_RADIO_FQNAME, &status) : NULL;
	if (!obj) {
		printf("no %s (%d)\n", SEH_RADIO_FQNAME, status);
		return -1;
	}
	c = gbinder_client_new(obj, SEH_RADIO_2_0);
	req = gbinder_client_new_request(c);
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_int32(&w, 0x5339);	/* serial */
	args(&w, data);
	ret = gbinder_client_transact_sync_oneway(c, code, req);
	gbinder_local_request_unref(req);
	gbinder_client_unref(c);
	gbinder_servicemanager_unref(sm);
	return ret;
}

static void mobile_data_args(GBinderWriter *w, gpointer data)
{
	gbinder_writer_append_bool(w, GPOINTER_TO_INT(data));	/* enabled */
	gbinder_writer_append_bool(w, FALSE);			/* roaming */
}

static int mobile_data(const char *dev, gboolean on)
{
	int ret = seh_transact(dev, SEH_TX_SET_MOBILE_DATA, mobile_data_args, GINT_TO_POINTER(on));

	printf("mobile data %s: %s\n", on ? "on" : "off", ret ? "failed" : "sent");
	return ret ? 1 : 0;
}

static int dds(const char *dev, int phone)
{
	GBinderServiceManager *sm = gbinder_servicemanager_new(dev);
	GBinderRemoteObject *obj;
	GBinderClient *c;
	GBinderLocalRequest *req;
	GBinderWriter w;
	gint32 *param;
	int status = 0, ret;

	obj = sm ? gbinder_servicemanager_get_service_sync(sm, SEH_RADIO_FQNAME, &status) : NULL;
	if (!obj) {
		printf("data slot: no %s (%d)\n", SEH_RADIO_FQNAME, status);
		return 1;
	}
	c = gbinder_client_new(obj, SEH_RADIO_2_0);
	req = gbinder_client_new_request(c);
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_int32(&w, 0x5339);	/* serial */
	gbinder_writer_append_bool(&w, TRUE);
	param = gbinder_writer_new0(&w, gint32);
	*param = phone;
	gbinder_writer_append_buffer_object(&w, param, sizeof(*param));
	ret = gbinder_client_transact_sync_oneway(c, SEH_TX_SET_DATA_ALLOWED, req);
	printf("data slot %d: %s\n", phone, ret ? "failed" : "sent");
	gbinder_local_request_unref(req);
	gbinder_client_unref(c);
	gbinder_servicemanager_unref(sm);
	return ret ? 1 : 0;
}

/* ---- simple requests ---- */

static void fail_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		      RADIO_ERROR err, const GBinderReader *args, gpointer data);

static void generic_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
			 RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	(void)req; (void)resp; (void)args;
	if (check(st, err, data))
		printf("%s: ok\n", (const char *)data);
	if (g_object_get_data(G_OBJECT(client), "stay"))
		return;
	if (!strcmp(data, "hangup"))
		submit(RADIO_REQ_GET_LAST_CALL_FAIL_CAUSE, fail_done);
	else
		g_main_loop_quit(loop);
}

static void power(gboolean on)
{
	GBinderWriter w;
	RadioRequest *req = radio_request_new(client, RADIO_REQ_SET_RADIO_POWER, &w,
					      generic_done, NULL, on ? "radio on" : "radio off");

	gbinder_writer_append_bool(&w, on);
	radio_request_set_timeout(req, 30000);
	radio_request_submit(req);
	radio_request_unref(req);
}

static void mode(int type)
{
	GBinderWriter w;
	RadioRequest *req = radio_request_new(client, RADIO_REQ_SET_PREFERRED_NETWORK_TYPE, &w,
					      generic_done, NULL, "network mode");

	gbinder_writer_append_int32(&w, type);	/* RADIO_PREF_NET_TYPE */
	radio_request_set_timeout(req, 40000);
	radio_request_submit(req);
	radio_request_unref(req);
}

static void pin(const char *code)
{
	GBinderWriter w;
	RadioRequest *req = radio_request_new(client, RADIO_REQ_SUPPLY_ICC_PIN_FOR_APP, &w,
					      generic_done, NULL, "pin");

	gbinder_writer_append_hidl_string(&w, code);
	gbinder_writer_append_hidl_string(&w, "");	/* aid: the default app */
	radio_request_submit(req);
	radio_request_unref(req);
}

static void hangup(void)
{
	RadioRequest *req = radio_request_new(client,
		RADIO_REQ_HANGUP_FOREGROUND_RESUME_BACKGROUND, NULL, generic_done, NULL, "hangup");

	radio_request_submit(req);
	radio_request_unref(req);
}

static const char *call_state[] = { "active", "holding", "dialing", "alerting",
				    "incoming", "waiting" };

static void calls_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		       RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	gsize count = 0, size = 0;
	const guint8 *calls;

	(void)req; (void)resp; (void)data;
	if (!check(st, err, "calls"))
		return;
	gbinder_reader_copy(&r, args);
	/* vec<Call_1_2>: the element size comes with the vector */
	calls = gbinder_reader_read_hidl_vec(&r, &count, &size);
	if (!count)
		printf("calls: none\n");
	else
		printf("calls: %zu, first %s (index %d)\n", count,
		       NAME(call_state, ((const gint32 *)calls)[0]), ((const gint32 *)calls)[1]);
}

static gboolean poll_calls(gpointer data)
{
	(void)data;
	submit(RADIO_REQ_GET_CURRENT_CALLS, calls_done);
	return G_SOURCE_CONTINUE;
}

static void fail_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
		      RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	GBinderReader r;
	const gint32 *v;
	gsize size;

	(void)req; (void)resp; (void)data;
	if (check(st, err, "last call fail cause")) {
		gbinder_reader_copy(&r, args);
		v = read_struct(&r, &size);
		if (v)
			printf("last call fail cause: %d\n", v[0]);
	}
	g_main_loop_quit(loop);
}

static gboolean hangup_cb(gpointer data)
{
	(void)data;
	g_object_set_data(G_OBJECT(client), "stay", NULL);
	hangup();
	return G_SOURCE_REMOVE;
}

static void dial(const char *number)
{
	GBinderWriter w;
	GBinderParent parent;
	RadioDial *d;
	RadioRequest *req = radio_request_new(client, RADIO_REQ_DIAL, &w,
					      generic_done, NULL, "dial");

	d = gbinder_writer_new0(&w, RadioDial);
	d->address.data.str = gbinder_writer_strdup(&w, number);
	d->address.len = strlen(number);
	d->address.owns_buffer = TRUE;
	d->clir = RADIO_CLIR_DEFAULT;
	parent.index = gbinder_writer_append_buffer_object(&w, d, sizeof(*d));
	parent.offset = G_STRUCT_OFFSET(RadioDial, address.data.str);
	gbinder_writer_append_buffer_object_with_parent(&w, d->address.data.str,
							d->address.len + 1, &parent);
	/* uusInfo is empty, but its buffer is still written */
	parent.offset = G_STRUCT_OFFSET(RadioDial, uusInfo.data.ptr);
	gbinder_writer_append_buffer_object_with_parent(&w, "", 0, &parent);
	radio_request_set_timeout(req, 30000);
	radio_request_submit(req);
	radio_request_unref(req);
}

static void indication(RadioClient *c, RADIO_IND code, const GBinderReader *args,
		       gpointer data)
{
	GBinderReader r;
	const gint32 *v;
	gsize size;

	(void)data;
	printf("indication: %s\n", radio_client_ind_name(c, code));
	if (code == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4 && args) {
		/* SignalStrength_1_4: gsm 3, cdma 2, evdo 3, lte 6, tdscdma 3, wcdma 4, nr 6 */
		gbinder_reader_copy(&r, args);
		v = read_struct(&r, &size);
		if (v && size >= 27 * 4)
			printf("signal: lte rssi %d rsrp -%d rsrq -%d rssnr %d | wcdma rscp %d ecno %d\n",
			       v[8], v[9], v[10], v[11], v[19], v[20]);
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/hwbinder";
	RadioInstance *radio;
	const char *cmd;
	int secs = 0;

	if (argc > 2 && !strcmp(argv[1], "-d")) {
		dev = argv[2];
		argc -= 2; argv += 2;
	}
	cmd = argc > 1 ? argv[1] : "status";

	radio = radio_instance_new_with_version(dev, "slot1", RADIO_INTERFACE_1_4);
	if (!radio) {
		fprintf(stderr, "no IRadio slot1 on %s\n", dev);
		return 2;
	}
	client = radio_client_new(radio);
	radio_client_add_indication_handler(client, RADIO_IND_ANY, indication, NULL);
	loop = g_main_loop_new(NULL, FALSE);
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (!strcmp(cmd, "status")) {
		status_next(0);
	} else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
		power(!strcmp(cmd, "on"));
	} else if (!strcmp(cmd, "pin") && argc > 2) {
		pin(argv[2]);
	} else if (!strcmp(cmd, "dial") && argc > 2) {
		secs = argc > 3 ? atoi(argv[3]) : 30;
		g_object_set_data(G_OBJECT(client), "stay", GINT_TO_POINTER(1));
		dial(argv[2]);
		g_timeout_add_seconds(3, poll_calls, NULL);
		g_timeout_add_seconds(secs, hangup_cb, NULL);
	} else if (!strcmp(cmd, "attach") && argc > 2) {
		attach_apn(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
	} else if (!strcmp(cmd, "data") && argc > 2) {
		setup_data(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
	} else if (!strcmp(cmd, "mobiledata")) {
		return mobile_data(dev, argc < 3 || strcmp(argv[2], "off"));
	} else if (!strcmp(cmd, "dds")) {
		return dds(dev, argc > 2 ? atoi(argv[2]) : 0);
	} else if (!strcmp(cmd, "ids")) {
		ids();
	} else if (!strcmp(cmd, "mode") && argc > 2) {
		mode(atoi(argv[2]));
	} else if (!strcmp(cmd, "hangup")) {
		hangup();
	} else if (!strcmp(cmd, "watch")) {
		secs = argc > 2 ? atoi(argv[2]) : 30;
		g_timeout_add_seconds(secs, quit_cb, NULL);
	} else {
		fprintf(stderr, "usage: s9p-radio [-d dev] status|on|off|pin N|dial N [s]|hangup|watch [s]\n");
		return 2;
	}
	/* never wait forever */
	g_timeout_add_seconds(secs + 60, quit_cb, NULL);
	g_main_loop_run(loop);

	radio_client_unref(client);
	radio_instance_unref(radio);
	g_main_loop_unref(loop);
	return exit_code;
}
