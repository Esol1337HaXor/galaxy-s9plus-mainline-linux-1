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

/* ---- simple requests ---- */

static void generic_done(RadioRequest *req, RADIO_TX_STATUS st, RADIO_RESP resp,
			 RADIO_ERROR err, const GBinderReader *args, gpointer data)
{
	(void)req; (void)resp; (void)args;
	if (check(st, err, data))
		printf("%s: ok\n", (const char *)data);
	if (!g_object_get_data(G_OBJECT(client), "stay"))
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
	(void)c; (void)args; (void)data;
	printf("indication: %s\n", radio_client_ind_name(c, code));
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
		g_timeout_add_seconds(secs, hangup_cb, NULL);
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
