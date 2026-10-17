// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung's ISehRadio calls (vendor.samsung.hardware.radio@2.0, served by
 * libsec-ril next to IRadio) and the data profile writer.
 *
 * The transaction codes and argument layouts are from the HIDL proxy in the
 * phone's own vendor.samsung.hardware.radio@2.0.so (BpHwSehRadio::_hidl_*).
 * The calls are oneway; their answers go to ISehRadioResponse, which nobody
 * registers here, so only the RIL's log shows them.
 */
#include "s9p-ril.h"

#include <gbinder_client.h>
#include <gbinder_local_request.h>
#include <gbinder_remote_object.h>
#include <gbinder_servicemanager.h>

#include <string.h>

#define SEH_RADIO_FQNAME	"vendor.samsung.hardware.radio@2.1::ISehRadio/slot1"
#define SEH_RADIO_2_0		"vendor.samsung.hardware.radio@2.0::ISehRadio"

#define SEH_TX_SET_DATA_ALLOWED		38	/* (serial, bool, SehAllowDataParam) */
#define SEH_TX_SET_MOBILE_DATA		39	/* (serial, bool enabled, bool roaming) */
#define SEH_TX_SEND_REQUEST_RAW		0x28	/* (serial, vec<uint8>) */

typedef void (*seh_args_fn)(GBinderWriter *w, gconstpointer data);

static int seh_transact(const char *dev, guint32 code, seh_args_fn args, gconstpointer data)
{
	GBinderServiceManager *sm = gbinder_servicemanager_new(dev);
	GBinderRemoteObject *obj;
	GBinderClient *c;
	GBinderLocalRequest *req;
	GBinderWriter w;
	int status = 0, ret;

	obj = sm ? gbinder_servicemanager_get_service_sync(sm, SEH_RADIO_FQNAME, &status) : NULL;
	if (!obj) {
		if (sm)
			gbinder_servicemanager_unref(sm);
		return -1;
	}
	c = gbinder_client_new(obj, SEH_RADIO_2_0);
	req = gbinder_client_new_request(c);
	gbinder_local_request_init_writer(req, &w);
	gbinder_writer_append_int32(&w, 0x5339);	/* serial: nobody reads the answer */
	args(&w, data);
	ret = gbinder_client_transact_sync_oneway(c, code, req);
	gbinder_local_request_unref(req);
	gbinder_client_unref(c);
	gbinder_servicemanager_unref(sm);
	return ret;
}

/*
 * IRadio's setDataAllowed reaches libsec-ril (SET_ALLOW_DATA) without a
 * phone id; it then writes ril.dds.data.slotid=-1, never takes this slot as
 * the data one and rejects every data call ("isDdsRil(0)").
 * SehAllowDataParam is { int32 defaultDataPhoneId }.
 */
static void data_slot_args(GBinderWriter *w, gconstpointer data)
{
	gint32 *param;

	gbinder_writer_append_bool(w, TRUE);
	param = gbinder_writer_new0(w, gint32);
	*param = GPOINTER_TO_INT(data);
	gbinder_writer_append_buffer_object(w, param, sizeof(*param));
}

int s9p_seh_set_data_slot(const char *dev, int slot)
{
	return seh_transact(dev, SEH_TX_SET_DATA_ALLOWED, data_slot_args, GINT_TO_POINTER(slot));
}

static void mobile_data_args(GBinderWriter *w, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);

	gbinder_writer_append_bool(w, flags & 1);
	gbinder_writer_append_bool(w, flags & 2);
}

int s9p_seh_mobile_data(const char *dev, gboolean on, gboolean roaming)
{
	return seh_transact(dev, SEH_TX_SET_MOBILE_DATA, mobile_data_args,
			    GUINT_TO_POINTER((on ? 1 : 0) | (roaming ? 2 : 0)));
}

struct bytes {
	const guint8 *data;
	gsize len;
};

static void raw_args(GBinderWriter *w, gconstpointer data)
{
	const struct bytes *b = data;

	gbinder_writer_append_hidl_vec(w, gbinder_writer_memdup(w, b->data, b->len), b->len, 1);
}

int s9p_seh_raw(const char *dev, const guint8 *data, gsize len)
{
	struct bytes b = { data, len };

	return seh_transact(dev, SEH_TX_SEND_REQUEST_RAW, raw_args, &b);
}

static void set_string(GBinderWriter *w, GBinderHidlString *f, const char *str)
{
	f->data.str = gbinder_writer_strdup(w, str);
	f->len = strlen(str);
	f->owns_buffer = TRUE;
}

static void put_string(GBinderWriter *w, guint idx, const GBinderHidlString *f, gsize offset)
{
	GBinderParent parent = { .index = idx, .offset = offset };

	gbinder_writer_append_buffer_object_with_parent(w, f->data.str, f->len + 1, &parent);
}

void s9p_write_data_profile(GBinderWriter *w, const char *apn, const char *user,
			    const char *pass, RADIO_PDP_PROTOCOL_TYPE protocol)
{
	RadioDataProfile_1_4 *dp = gbinder_writer_new0(w, RadioDataProfile_1_4);
	guint idx;

	dp->profileId = RADIO_DATA_PROFILE_DEFAULT;
	set_string(w, &dp->apn, apn);
	dp->protocol = dp->roamingProtocol = protocol;
	dp->authType = *user ? RADIO_APN_AUTH_PAP_CHAP : RADIO_APN_AUTH_NONE;
	set_string(w, &dp->user, user);
	set_string(w, &dp->password, pass);
	dp->type = 1;	/* 3GPP */
	dp->enabled = TRUE;
	dp->supportedApnTypesBitmap = RADIO_APN_TYPE_DEFAULT;
	dp->preferred = dp->persistent = TRUE;
	idx = gbinder_writer_append_buffer_object(w, dp, sizeof(*dp));
	put_string(w, idx, &dp->apn, G_STRUCT_OFFSET(RadioDataProfile_1_4, apn.data.str));
	put_string(w, idx, &dp->user, G_STRUCT_OFFSET(RadioDataProfile_1_4, user.data.str));
	put_string(w, idx, &dp->password, G_STRUCT_OFFSET(RadioDataProfile_1_4, password.data.str));
}
