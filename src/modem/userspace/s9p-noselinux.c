// SPDX-License-Identifier: GPL-2.0
/*
 * LD_PRELOAD for the Android processes of extras/modem (init, the service
 * managers, rild), built against bionic. Ubuntu's kernel has no SELinux, so
 * libselinux fails every call and Android refuses to start services or
 * register binder services. Here everything is allowed, permissive, and
 * labelled with a made-up but consistent context; nothing is enforced.
 *
 * Only the entry points those binaries import are replaced; the rest of
 * libselinux is used as it is.
 */
typedef unsigned long size_t;
typedef int pid_t;
typedef unsigned short security_class_t;

extern char *strdup(const char *);
extern size_t strlen(const char *);

#define EXPORT __attribute__((visibility("default")))
#define CTX_PROC "u:r:s9p:s0"
#define CTX_CHILD "u:r:s9p_service:s0"	/* differs from init's, see Service::Start */
#define CTX_FILE "u:object_r:s9p_file:s0"

static int ret_ctx(char **con, const char *ctx)
{
	if (con)
		*con = strdup(ctx);
	return 0;
}

/* the get*filecon family returns the context length on success */
static int ret_file(char **con)
{
	if (con)
		*con = strdup(CTX_FILE);
	return (int)strlen(CTX_FILE) + 1;
}

EXPORT int getcon(char **con) { return ret_ctx(con, CTX_PROC); }
EXPORT int getpidcon(pid_t pid, char **con) { (void)pid; return ret_ctx(con, CTX_PROC); }
EXPORT int getpeercon(int fd, char **con) { (void)fd; return ret_ctx(con, CTX_PROC); }
EXPORT int getfilecon(const char *p, char **con) { (void)p; return ret_file(con); }
EXPORT int lgetfilecon(const char *p, char **con) { (void)p; return ret_file(con); }
EXPORT int fgetfilecon(int fd, char **con) { (void)fd; return ret_file(con); }

EXPORT int setexeccon(const char *c) { (void)c; return 0; }
EXPORT int setfscreatecon(const char *c) { (void)c; return 0; }
EXPORT int setsockcreatecon(const char *c) { (void)c; return 0; }
EXPORT int setcon(const char *c) { (void)c; return 0; }
EXPORT int lsetfilecon(const char *p, const char *c) { (void)p; (void)c; return 0; }
EXPORT int setfilecon(const char *p, const char *c) { (void)p; (void)c; return 0; }
EXPORT int fsetfilecon(int fd, const char *c) { (void)fd; (void)c; return 0; }

EXPORT int security_compute_create(const char *s, const char *t, security_class_t c, char **con)
{
	(void)s; (void)t; (void)c;
	return ret_ctx(con, CTX_CHILD);
}
EXPORT int security_getenforce(void) { return 0; }
EXPORT int security_setenforce(int v) { (void)v; return 0; }
EXPORT int is_selinux_enabled(void) { return 1; }
EXPORT int selinux_check_access(const char *s, const char *t, const char *c,
				const char *p, void *a)
{
	(void)s; (void)t; (void)c; (void)p; (void)a;
	return 0;
}
EXPORT int selinux_get_policy_version(void) { return 30; }
EXPORT security_class_t string_to_security_class(const char *n) { (void)n; return 1; }
EXPORT int selinux_android_restorecon(const char *p, unsigned int f) { (void)p; (void)f; return 0; }
EXPORT int selinux_android_load_policy(void) { return 0; }
EXPORT int selinux_android_load_policy_from_fd(int fd, const char *d) { (void)fd; (void)d; return 0; }
EXPORT int selinux_status_open(int fallback) { (void)fallback; return 0; }
EXPORT int selinux_status_updated(void) { return 0; }

/* label handles: one dummy object, every lookup answers the same label */
static int dummy_handle;
EXPORT void *selinux_android_file_context_handle(void) { return &dummy_handle; }
EXPORT void *selinux_android_service_context_handle(void) { return &dummy_handle; }
EXPORT void *selinux_android_hw_service_context_handle(void) { return &dummy_handle; }
EXPORT void *selinux_android_vendor_service_context_handle(void) { return &dummy_handle; }
EXPORT void selinux_android_set_sehandle(void *h) { (void)h; }
EXPORT int selabel_lookup(void *h, char **con, const char *key, int type)
{
	(void)h; (void)key; (void)type;
	return ret_ctx(con, CTX_FILE);
}
EXPORT int selabel_lookup_best_match(void *h, char **con, const char *key,
				     const char **aliases, int type)
{
	(void)h; (void)key; (void)aliases; (void)type;
	return ret_ctx(con, CTX_FILE);
}
EXPORT void selabel_close(void *h) { (void)h; }
